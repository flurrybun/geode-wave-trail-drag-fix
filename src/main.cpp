#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>


#define MBO(object, offset, type) (*reinterpret_cast<type*>(reinterpret_cast<uintptr_t>(object) + offset))

using namespace geode::prelude;

class $modify(WTDFPlayerObject, PlayerObject) {

  struct Fields {
  
    // why is there so much state to keep track of
    // this used to be so simple

    // a point would not have been otherwise added, but force it to be added
    bool forceAdd = true;

    // the pad has the same effect as the ring, except it has to be done 1 update later
    bool spiderPadTriggered = false;
    // the points that these 2 add are different from standard forceAdd, and they take precedence over it
    bool forceAddSpiderPad = false;
    bool forceAddSpiderRing = false;

    bool justTeleported = false;
    bool teleportedPreviouslySpiderRing = false;
    bool transitionToCollision = false;
    bool wasOnGround = false;
    float portalTargetLine;
    CCPoint previousPos{-12, -12};
    CCPoint currentPos{-12, -12};
    CCPoint nextPosNoCollision{-12, -12};

    GameObject* collidedSlope = nullptr;
  };

  void resetObject() {
    m_waveTrail->reset();
    PlayerObject::resetObject();
  }
  
  void update(float deltaTime) {
    // i used to just use m_position as the current position, but apparently that only gets updated once per frame
    // this became really apparent once Click Between Frames came out, so i guess i am going back to doing it this way
    // i would have had to do this anyway to get nextPosNoCollision, so yeah
    m_fields->currentPos = getRealPosition();
    PlayerObject::update(deltaTime);
    m_fields->nextPosNoCollision = getRealPosition();
    // if the player collides with a slope, this will stop being nullptr
    m_fields->collidedSlope = nullptr;
  }
  
  bool preSlopeCollision(float p0, GameObject* p1) {
    bool value = PlayerObject::preSlopeCollision(p0, p1);
    if (!value) m_fields->collidedSlope = p1;
    return value;
  }

  void postCollision(float deltaTime) {

    bool hitBottom = MBO(this, 0x750, double) != 0;
    bool hitTop = MBO(this, 0x748, double) != 0;
    
    PlayerObject::postCollision(deltaTime);

    if (LevelEditorLayer::get() || !m_gameLayer) return;
    
    if (!m_isDart || m_isHidden) {
      m_fields->previousPos = m_fields->currentPos;
      return;
    }
    
    CCPoint previousPosition = m_fields->previousPos;
    CCPoint currentPosition = m_fields->currentPos;
    CCPoint nextPositionNoCollision = m_fields->nextPosNoCollision;
    CCPoint nextPosition = getRealPosition();

    bool wasOnGround = m_fields->wasOnGround;
    m_fields->wasOnGround = m_isOnGround;


    if (m_fields->justTeleported) {
      // if we just teleported, we set a streak point to the portal's position and save its location
      // we need to place a new point if we are clicking a spider orb and teleporting at the same time
      // as well
      m_fields->justTeleported = false;
      m_fields->teleportedPreviouslySpiderRing = m_fields->forceAddSpiderRing;
      m_fields->forceAddSpiderRing = false;
      m_fields->forceAddSpiderPad = false;
      m_fields->spiderPadTriggered = false;
      m_fields->forceAdd = false;
      m_fields->portalTargetLine = m_isSideways ? previousPosition.y : previousPosition.x;
      addWaveTrailPoint(previousPosition);
      return;
    } else if (m_fields->forceAdd && !m_fields->forceAddSpiderRing && !m_fields->forceAddSpiderPad) {
      m_fields->forceAdd = false;
      addWaveTrailPoint(currentPosition);
      m_fields->previousPos = currentPosition;
      return;
    } else if (m_fields->spiderPadTriggered) {
      m_fields->spiderPadTriggered = false;
      m_fields->forceAddSpiderPad = true;
      m_fields->previousPos = currentPosition;
      return;
    } else if (m_fields->forceAddSpiderRing || m_fields->forceAddSpiderPad) {
      // spider orbs and pads require special care so the line looks straight
      m_fields->forceAddSpiderRing = false;
      m_fields->forceAddSpiderPad = false;
      m_fields->forceAdd = false;
      m_fields->transitionToCollision = false;
      m_fields->teleportedPreviouslySpiderRing = false;
      CCPoint pointToAdd = m_isSideways ? CCPoint{nextPosition.x, previousPosition.y} : CCPoint{previousPosition.x, nextPosition.y};
      addWaveTrailPoint(pointToAdd);
      m_fields->previousPos = pointToAdd;
      return;
    } else if (m_fields->teleportedPreviouslySpiderRing) {
      m_fields->teleportedPreviouslySpiderRing = false;
      CCPoint pointToAdd = m_isSideways ? CCPoint{nextPosition.x, m_fields->portalTargetLine} : CCPoint{m_fields->portalTargetLine, nextPosition.y};
      addWaveTrailPoint(pointToAdd);
      m_fields->previousPos = pointToAdd;
      return;
    }

    // 'teleport slopes' (stacked slopes that teleport the player)
    // don't work normally, this is a special case for them
    // prevWasOnSlope is required so that this doesn't fire
    // when on top of a slope, which rapidly collides/uncollides
    // with the slope every update
    // this also fires if you just collide with a slope normally
    // but that doesn't mess anything up too badly
    /*
    if (m_isOnSlope && !m_wasOnSlope && !prevWasOnSlope) {
      CCPoint pointToAdd{nextPosition.x, currentPosition.y};
      addWaveTrailPoint(pointToAdd);
      m_fields->previousPos = pointToAdd;
      // force the add on the next update, when the player actually
      // gets teleported to the top of the slope stack, so, a line from
      // the point where the player hit the slope stack, to the top of the
      // slope stack
      m_fields->forceAdd = true;
      return;
    }*/

    cocos2d::CCPoint currentVector = (nextPosition - currentPosition).normalize();
    cocos2d::CCPoint previousVector = (currentPosition - previousPosition).normalize();
    float crossProductMagnitude = abs(currentVector.cross(previousVector));
    
    // make the error margin inversely proportional to the delta time
    // if 2 updates are extremely close together calculating the angle between them may lead to inaccuracies
    // making the error margin larger the closer 2 updates are makes the chance of a false positive smaller
    // for some reason, deltaTime always seems to be 0.25 even with physics bypass set to something other
    // than 240. the only time that it isn't 0.25 is when click between frames sends its updates.
    // this is kind of weird, i would expect physics bypass to affect this, but i guess not
    float errorMargin = 0.004/deltaTime;

    // save the current point as prevPoint only if it is placed as a streak point
    // this makes it so that even smooth paths where
    // any 3 consecutive points may look like a straight line
    // still get marked for new point addition, because
    // we are no longer checking for 3 consecutive points
    // this accounts for the wave going in very changing smooth curves
    // but one issue is that the cumulative error eventually gets large enough
    // that a point is placed even when moving in a constant direction
    // i don't think that can be recreated unless you use the zoom trigger to
    // zoom the game out up to a point where you can't even notice that extra point anymore though
    // basically, this trades off accounting for one type of cumulative error
    // (slowly moving smooth transitions not being detected as actually changing moving)
    // over another (the an unchanged direction being detected as a change in direction over a long period of time)
    if (crossProductMagnitude <= errorMargin) return;
    
    if (nextPosition != nextPositionNoCollision && !m_fields->transitionToCollision) {
      // if the player is supposed to land on a block between 2 updates, this calculates
      // where the player would have landed, and adds that as a point rather than the current position
      if (m_isSideways) {
        std::swap(nextPositionNoCollision.x, nextPositionNoCollision.y);
        std::swap(nextPosition.x, nextPosition.y);
        std::swap(currentPosition.x, currentPosition.y);
      }
      float objectBoundMinX = std::numeric_limits<float>::lowest();
      float objectBoundMaxX = std::numeric_limits<float>::max();
      float objectBoundMinY = std::numeric_limits<float>::lowest();
      float objectBoundMaxY = std::numeric_limits<float>::max();

      float hitboxSize = getObjectRect().size.width/2;

      float playerConst = currentPosition.x + (m_isGoingLeft ? 2.f : -2.f);

      if (m_fields->collidedSlope) {
        if (m_isSideways) {
          objectBoundMinX = m_fields->collidedSlope->getObjectRect().getMinY();
          objectBoundMaxX = m_fields->collidedSlope->getObjectRect().getMaxY();
          objectBoundMinY = m_fields->collidedSlope->getObjectRect().getMinX();
          objectBoundMaxY = m_fields->collidedSlope->getObjectRect().getMaxX();
        } else {
          objectBoundMinX = m_fields->collidedSlope->getObjectRect().getMinX();
          objectBoundMaxX = m_fields->collidedSlope->getObjectRect().getMaxX();
          objectBoundMinY = m_fields->collidedSlope->getObjectRect().getMinY();
          objectBoundMaxY = m_fields->collidedSlope->getObjectRect().getMaxY();
        }
      } else if (m_collidedObject) {
        if (m_isSideways) {
          objectBoundMinX = m_collidedObject->getObjectRect().getMinY();
          objectBoundMaxX = m_collidedObject->getObjectRect().getMaxY();
          objectBoundMinY = m_collidedObject->getObjectRect().getMinX();
          objectBoundMaxY = m_collidedObject->getObjectRect().getMaxX();
        } else {
          objectBoundMinX = m_collidedObject->getObjectRect().getMinX();
          objectBoundMaxX = m_collidedObject->getObjectRect().getMaxX();
          objectBoundMinY = m_collidedObject->getObjectRect().getMinY();
          objectBoundMaxY = m_collidedObject->getObjectRect().getMaxY();
        }
      }

      float intercept = (nextPositionNoCollision.y - currentPosition.y)/(nextPositionNoCollision.x - currentPosition.x);
      CCPoint intersectionPoint;
      if (!m_fields->collidedSlope) {
        intersectionPoint.x = (nextPosition.y - currentPosition.y + intercept * currentPosition.x)/intercept;
        intersectionPoint.y = nextPosition.y;
        
        // this happens if the player snapped to an object
        if (intersectionPoint.x <= objectBoundMinX - hitboxSize && !m_isGoingLeft) {
          float newIntersectionY = intercept * (objectBoundMinX - hitboxSize - currentPosition.x) + currentPosition.y;
          if (m_isSideways)
            addWaveTrailPoint({newIntersectionY, objectBoundMinX - hitboxSize});
          else
            addWaveTrailPoint({objectBoundMinX - hitboxSize, newIntersectionY});
          
          intersectionPoint.x = objectBoundMinX - hitboxSize;
          intersectionPoint.y = nextPosition.y;
        }
        else if (intersectionPoint.x >= objectBoundMaxX + hitboxSize && m_isGoingLeft) {
          float newIntersectionY = intercept * (objectBoundMaxX + hitboxSize - currentPosition.x) + currentPosition.y;
          if (m_isSideways)
            addWaveTrailPoint({newIntersectionY, objectBoundMaxX + hitboxSize});
          else
            addWaveTrailPoint({objectBoundMaxX + hitboxSize, newIntersectionY});
          
          intersectionPoint.x = objectBoundMaxX + hitboxSize;
          intersectionPoint.y = nextPosition.y;
        }
      } else {
        if (m_isOnGround || wasOnGround) intersectionPoint = currentPosition;
        else {
          float slopeAngle = m_unk518;
          float slopeRotation = m_fields->collidedSlope->getRotation() * M_PI / 180;
          int slopeFlip = 2 * (m_fields->collidedSlope->isFlipX() == m_fields->collidedSlope->isFlipY()) - 1;
          float slopeIntercept = slopeFlip * tan(slopeAngle + slopeRotation);
          intersectionPoint.x = (slopeIntercept*nextPosition.x - intercept*currentPosition.x - nextPosition.y + currentPosition.y)/(slopeIntercept - intercept);
          intersectionPoint.y = slopeIntercept * (intersectionPoint.x - nextPosition.x) + nextPosition.y;
          
          if (
            (
              (currentPosition.y - hitboxSize <= objectBoundMinY && hitBottom) ||
              (currentPosition.y + hitboxSize >= objectBoundMaxY && hitTop)
            ) && !m_isGoingLeft && !m_wasOnSlope
          ) {
            float newIntersectionY = intercept * (objectBoundMinX - hitboxSize - currentPosition.x) + currentPosition.y;
            if (m_isSideways)
              addWaveTrailPoint({newIntersectionY, objectBoundMinX - hitboxSize});
            else
              addWaveTrailPoint({objectBoundMinX - hitboxSize, newIntersectionY});
            
            intersectionPoint.x = objectBoundMinX - hitboxSize;
            intersectionPoint.y = nextPosition.y;
          } else if (
            (
              (currentPosition.y - hitboxSize <= objectBoundMinY && hitBottom) ||
              (currentPosition.y + hitboxSize <= objectBoundMaxY && hitTop)
            ) && m_isGoingLeft && !m_wasOnSlope
          ) {
            float newIntersectionY = intercept * (objectBoundMaxX + hitboxSize - currentPosition.x) + currentPosition.y;
            if (m_isSideways)
              addWaveTrailPoint({newIntersectionY, objectBoundMaxX + hitboxSize});
            else
              addWaveTrailPoint({objectBoundMaxX + hitboxSize, newIntersectionY});
            
            intersectionPoint.x = objectBoundMaxX + hitboxSize;
            intersectionPoint.y = nextPosition.y;
          }
        }
      }
      if (m_isSideways) {
        std::swap(nextPositionNoCollision.x, nextPositionNoCollision.y);
        std::swap(nextPosition.x, nextPosition.y);
        std::swap(currentPosition.x, currentPosition.y);
        std::swap(intersectionPoint.x, intersectionPoint.y);
      }
      addWaveTrailPoint(intersectionPoint);
      m_fields->previousPos = intersectionPoint;
      m_fields->transitionToCollision = false;
    } else {
      if (nextPosition == nextPositionNoCollision) m_fields->transitionToCollision = false;
      addWaveTrailPoint(currentPosition);
      m_fields->previousPos = currentPosition;
    }
  }

  // this will add the requested point to the wave trail, and check if it is (almost) colinear with the last 2 points
  // if it is, it will remove the last point before adding the requested one, because there is no need to have a
  // colinear duplicate
  inline void addWaveTrailPoint(CCPoint point) {
    size_t objectCount = m_waveTrail->m_pointArray->count();
    if (objectCount >= 3) {
      CCPoint point0 = static_cast<PointNode *>(m_waveTrail->m_pointArray->objectAtIndex(objectCount - 2))->m_point;
      CCPoint point1 = static_cast<PointNode *>(m_waveTrail->m_pointArray->objectAtIndex(objectCount - 1))->m_point;
      CCPoint point2 = point;
      CCPoint vector01 = (point1 - point0).normalize();
      CCPoint vector12 = (point2 - point1).normalize();
      float cpm = abs(vector12.cross(vector01));
      if (cpm <= 0.001) m_waveTrail->m_pointArray->removeObjectAtIndex(objectCount - 1);
    }
    m_waveTrail->addPoint(point);
  }
  
  // spider orb
  void pushButton(PlayerButton button) {
    const int TOGGLE_RING = 1594;
    const int TELEPORT_RING = 3027;
    const int SPIDER_RING = 3004;
    if (!m_isDart || !m_gameLayer || LevelEditorLayer::get()) return PlayerObject::pushButton(button);
    bool willTriggerSpiderRing = false;
    
    for (size_t i = 0; i < m_touchingRings->count(); i++) {
      RingObject *ring = static_cast<RingObject *>(m_touchingRings->objectAtIndex(i));
      switch (ring->m_objectID) {
        // these 2 seem to allow the click to reach the next ring
        case TOGGLE_RING:  // fallthrough
          if (ring->m_claimTouch) return PlayerObject::pushButton(button);
        case TELEPORT_RING:
          continue;
        case SPIDER_RING: // fallthrough
          willTriggerSpiderRing = true; // will trigger unless a toggle ring claims the touch
        default:
          if (!willTriggerSpiderRing) return PlayerObject::pushButton(button);
      }
    }
    if (willTriggerSpiderRing) {
      m_waveTrail->addPoint(m_fields->currentPos);
      m_fields->previousPos = m_fields->currentPos;
      m_fields->forceAddSpiderRing = true;
    }
    PlayerObject::pushButton(button);
  }
  
  // spider pad
  void spiderTestJump(bool p0) {
    if (!m_isDart || !m_gameLayer || LevelEditorLayer::get() || m_fields->justTeleported) return PlayerObject::spiderTestJump(p0);
    m_waveTrail->addPoint(m_fields->currentPos);
    m_fields->previousPos = m_fields->currentPos;
    // this runs on both spider orbs and pads triggering
    // this only needs to have an effect if a pad was triggered specifically
    if (!m_fields->forceAddSpiderRing) m_fields->spiderPadTriggered = true;
    PlayerObject::spiderTestJump(p0);
  }

  void doReversePlayer(bool state) {
    m_fields->forceAdd = true;
    PlayerObject::doReversePlayer(state);
  }

  void placeStreakPoint() { if (!m_isDart || !m_gameLayer) PlayerObject::placeStreakPoint(); }

  void toggleVisibility(bool state) {
    bool needsPoint = m_isHidden;
    PlayerObject::toggleVisibility(state);
    if (state && m_isDart && needsPoint) m_fields->forceAdd = true;
  }
};

class $modify(PlayLayer) {
  void playEndAnimationToPos(CCPoint pos) {
    if (m_player1 && m_player1->m_isDart)
      m_player1->m_waveTrail->addPoint(m_player1->getRealPosition()); 
    if (m_player2 && m_player2->m_isDart)
      m_player2->m_waveTrail->addPoint(m_player2->getRealPosition());
    PlayLayer::playEndAnimationToPos(pos);
  }
};

class $modify(GJBaseGameLayer) {
  // teleport portal
  void teleportPlayer(TeleportPortalObject *portal, PlayerObject *player) {
    GJBaseGameLayer::teleportPlayer(portal, player);
    // no idea why player can be null, but it happened in one level and thus caused a crash
    // it was a platformer level, if anyone has an explanation, let me know
    if (LevelEditorLayer::get() || !player || !player->m_isDart) return;
    CCPoint targetPos = getPortalTargetPos(portal, getPortalTarget(portal), player);
    static_cast<WTDFPlayerObject *>(player)->m_fields->previousPos = targetPos;
    static_cast<WTDFPlayerObject *>(player)->m_fields->justTeleported = true;
  }

  void toggleDualMode(GameObject* portal, bool state, PlayerObject* playerTouchingPortal, bool p4) {
    if (!state && playerTouchingPortal == m_player2) {
      // player 2 will become player 1 after the original executes, so copy the fields from player 2 and add them to player 1 afterward
      WTDFPlayerObject::Fields fieldsBackup = *static_cast<WTDFPlayerObject *>(m_player2)->m_fields.operator->();
      GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
      *static_cast<WTDFPlayerObject *>(m_player1)->m_fields.operator->() = fieldsBackup;
    } else {
      GJBaseGameLayer::toggleDualMode(portal, state, playerTouchingPortal, p4);
    }
  }
};

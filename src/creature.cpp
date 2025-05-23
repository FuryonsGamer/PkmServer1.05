// Icaro for upgrade, updates in code. based on tfs 1.4 
// Special Credits: Pota, Ruby
// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "creature.h"
#include "game.h"
#include "monster.h"
#include "events.h"
#include "configmanager.h"
#include "scheduler.h"

double Creature::speedA = 857.36;
double Creature::speedB = 261.29;
double Creature::speedC = -4795.01;

extern Game g_game;
extern ConfigManager g_config;
extern CreatureEvents* g_creatureEvents;
extern Events* g_events;

Creature::Creature()
{
	onIdleStatus();
	Nick = "";
}

Creature::~Creature()
{
	for (Creature* summon : summons) {
		summon->setAttackedCreature(nullptr);
		summon->setMaster(nullptr);
	}

	for (Condition* condition : conditions) {
		condition->endCondition(this);
	}

	for (auto condition : conditions) {
		delete condition;
	}
}

bool Creature::canSee(const Position& myPos, const Position& pos)
{
	if (myPos.z <= 7) {
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if (pos.z > 7) {
			return false;
		}
	} else if (myPos.z >= 8) {
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if (Position::getDistanceZ(myPos, pos) > 2) {
			return false;
		}
	}

	const int_fast32_t offsetz = myPos.getZ() - pos.getZ();

	return (pos.getX() >= myPos.getX() - (Map::maxViewportX - 2) + offsetz) && (pos.getX() <= myPos.getX() + (Map::maxViewportX - 2) + offsetz)
		&& (pos.getY() >= myPos.getY() - (Map::maxViewportY - 1) + offsetz) && (pos.getY() <= myPos.getY() + (Map::maxViewportY - 1) + offsetz);
}

bool Creature::canSee(const Position& pos) const
{
	return canSee(getPosition(), pos);
}

bool Creature::canSeeCreature(const Creature* creature) const
{
	//if (!canSeeGhostMode(creature) && creature->isInGhostMode()) {
	//	return false;
	//}

	if (!canSeeInvisibility() && creature->isInvisible()) {
		return false;
	}
	return true;
}

void Creature::setSkull(Skulls_t newSkull)
{
	skull = newSkull;
	g_game.updateCreatureSkull(this);
}

void Creature::setNameColor(NameColor_t newColor)
{
	nameColor = newColor;
	g_game.updateCreatureNameColor(this, newColor);
}

int64_t Creature::getTimeSinceLastMove() const
{
	if (lastStep) {
		return OTSYS_TIME() - lastStep;
	}
	return std::numeric_limits<int64_t>::max();
}

int32_t Creature::getWalkDelay(Direction dir) const
{
	if (lastStep == 0) {
		return 0;
	}

	int64_t ct = OTSYS_TIME();
	int64_t stepDuration = getStepDuration(dir);
	return stepDuration - (ct - lastStep);
}

int32_t Creature::getWalkDelay() const
{
	//Used for auto-walking
	if (lastStep == 0) {
		return 0;
	}

	int64_t ct = OTSYS_TIME();
	int64_t stepDuration = getStepDuration() * lastStepCost;
	return stepDuration - (ct - lastStep);
}

void Creature::onThink(uint32_t interval)
{
	if (!isMapLoaded && useCacheMap()) {
		isMapLoaded = true;
		updateMapCache();
	}

	if (followCreature && master != followCreature && !canSeeCreature(followCreature)) {
		onCreatureDisappear(followCreature, false);
	}

	if (attackedCreature && master != attackedCreature && !canSeeCreature(attackedCreature)) {
		onCreatureDisappear(attackedCreature, false);
	}

	blockTicks += interval;
	if (blockTicks >= 1000) {
		blockCount = std::min<uint32_t>(blockCount + 1, 2);
		blockTicks = 0;
	}

	if (followCreature) {
		walkUpdateTicks += interval;
		if (forceUpdateFollowPath || walkUpdateTicks >= 2000) {
			walkUpdateTicks = 0;
			forceUpdateFollowPath = false;
			isUpdatingPath = true;
		}
	}

	if (isUpdatingPath) {
		isUpdatingPath = false;
		//goToFollowCreature(); multithread
		g_game.addToCheckFollow(this);
	}

	if (hasCondition(CONDITION_FEAR)) {
		std::forward_list<Direction> dirList;
		startAutoWalk(dirList);
	}	


	//scripting event - onThink
	const CreatureEventList& thinkEvents = getCreatureEvents(CREATURE_EVENT_THINK);
	for (CreatureEvent* thinkEvent : thinkEvents) {
		thinkEvent->executeOnThink(this, interval);
	}
}

void Creature::onAttacking(uint32_t interval)
{
	if (!attackedCreature) {
		return;
	}

	onAttacked();
	attackedCreature->onAttacked();

	if (g_game.isSightClear(getPosition(), attackedCreature->getPosition(), true)) {
		doAttacking(interval);
	}
}

void Creature::onIdleStatus()
{
	if (getHealth() > 0) {
		if (this->isBossReturn()) {
			return;
		}
		damageMap.clear();
		lastHitCreatureId = 0;
	}
}

void Creature::onWalk()
{
	if (getWalkDelay() <= 0) {
		Direction dir;
		uint32_t flags = FLAG_IGNOREFIELDDAMAGE;
		if (getNextStep(dir, flags)) {
			
			ReturnValue ret = (canMove() && !hasCondition(CONDITION_STUN) && !hasCondition(CONDITION_SNARE)) ? g_game.internalMoveCreature(this, dir, flags) : RETURNVALUE_NOTPOSSIBLE;

			if (ret != RETURNVALUE_NOERROR) {
				if (Player* player = getPlayer()) {
					player->sendCancelMessage(ret);
					player->sendCancelWalk();
				}

				forceUpdateFollowPath = true;
			}
		} else {
			stopEventWalk();

			if (listWalkDir.empty()) {
				onWalkComplete();
			}

		}
	}

	if (cancelNextWalk) {
		listWalkDir.clear();
		onWalkAborted();
		cancelNextWalk = false;
	}

	if (eventWalk != 0) {
		eventWalk = 0;
		addEventWalk();
	}
}

void Creature::onWalk(Direction& dir)
{
	if (hasCondition(CONDITION_DRUNK) || hasCondition(CONDITION_CONFUSION)) {
		uint32_t r = uniform_random(0, 10);
		if (r <= DIRECTION_DIAGONAL_MASK) {
			if (r < DIRECTION_DIAGONAL_MASK) {
				dir = static_cast<Direction>(r);
			}

			// g_game.internalCreatureSay(this, TALKTYPE_MONSTER_SAY, "Hicks!", false);
		}
	}
}

bool Creature::getNextStep(Direction& dir, uint32_t&)
{
	if (listWalkDir.empty()) {
		return false;
	}

	dir = listWalkDir.front();
	listWalkDir.pop_front();
	onWalk(dir);
	return true;
}

void Creature::startAutoWalk(const std::forward_list<Direction>& listDir)
{
	listWalkDir = listDir;

	size_t size = 0;
	for (auto it = listDir.begin(); it != listDir.end() && size <= 1; ++it) {
		size++;
	}
	addEventWalk(size == 1);
}

void Creature::addEventWalk(bool firstStep)
{
	cancelNextWalk = false;

	if (getStepSpeed() <= 0) {
		return;
	}

	if (eventWalk != 0) {
		return;
	}

	int64_t ticks = getEventStepTicks(firstStep);
	if (ticks <= 0) {
		return;
	}

	// Take first step right away, but still queue the next
	if (ticks == 1) {
		g_game.checkCreatureWalk(getID());
	}

	eventWalk = g_scheduler.addEvent(createSchedulerTask(ticks, std::bind(&Game::checkCreatureWalk, &g_game, getID())));
}

void Creature::stopEventWalk()
{
	if (eventWalk != 0) {
		g_scheduler.stopEvent(eventWalk);
		eventWalk = 0;
	}
}

void Creature::updateMapCache()
{
	Tile* tile;
	const Position& myPos = getPosition();
	Position pos(0, 0, myPos.z);

	for (int32_t y = -maxWalkCacheHeight; y <= maxWalkCacheHeight; ++y) {
		for (int32_t x = -maxWalkCacheWidth; x <= maxWalkCacheWidth; ++x) {
			pos.x = myPos.getX() + x;
			pos.y = myPos.getY() + y;
			tile = g_game.map.getTile(pos);
			updateTileCache(tile, pos);
		}
	}
}

void Creature::updateTileCache(const Tile* tile, int32_t dx, int32_t dy)
{
	if (std::abs(dx) <= maxWalkCacheWidth && std::abs(dy) <= maxWalkCacheHeight) {
		localMapCache[maxWalkCacheHeight + dy][maxWalkCacheWidth + dx] = tile && tile->queryAdd(0, *this, 1, FLAG_PATHFINDING | FLAG_IGNOREFIELDDAMAGE) == RETURNVALUE_NOERROR;
	}
}

void Creature::updateTileCache(const Tile* tile, const Position& pos)
{
	const Position& myPos = getPosition();
	if (pos.z == myPos.z) {
		int32_t dx = Position::getOffsetX(pos, myPos);
		int32_t dy = Position::getOffsetY(pos, myPos);
		updateTileCache(tile, dx, dy);
	}
}

int32_t Creature::getWalkCache(const Position& pos) const
{
	if (!useCacheMap()) {
		return 2;
	}

	const Position& myPos = getPosition();
	if (myPos.z != pos.z) {
		return 0;
	}

	if (pos == myPos) {
		return 1;
	}

	int32_t dx = Position::getOffsetX(pos, myPos);
	if (std::abs(dx) <= maxWalkCacheWidth) {
		int32_t dy = Position::getOffsetY(pos, myPos);
		if (std::abs(dy) <= maxWalkCacheHeight) {
			if (localMapCache[maxWalkCacheHeight + dy][maxWalkCacheWidth + dx]) {
				return 1;
			} else {
				return 0;
			}
		}
	}

	//out of range
	return 2;
}

void Creature::onAddTileItem(const Tile* tile, const Position& pos)
{
	if (isMapLoaded && pos.z == getPosition().z) {
		updateTileCache(tile, pos);
	}
}

void Creature::onUpdateTileItem(const Tile* tile, const Position& pos, const Item*,
                                const ItemType& oldType, const Item*, const ItemType& newType)
{
	if (!isMapLoaded) {
		return;
	}

	if (oldType.blockSolid || oldType.blockPathFind || newType.blockPathFind || newType.blockSolid) {
		if (pos.z == getPosition().z) {
			updateTileCache(tile, pos);
		}
	}
}

void Creature::onRemoveTileItem(const Tile* tile, const Position& pos, const ItemType& iType, const Item*)
{
	if (!isMapLoaded) {
		return;
	}

	if (iType.blockSolid || iType.blockPathFind || iType.isGroundTile()) {
		if (pos.z == getPosition().z) {
			updateTileCache(tile, pos);
		}
	}
}

void Creature::onCreatureAppear(Creature* creature, bool isLogin)
{
	//std::cout << "CreatureName " << creature->getName() << " on MonsterName Screen: " << this->getName() << std::endl;
	if (creature == this) {
		if (useCacheMap()) {
			isMapLoaded = true;
			updateMapCache();
		}

		if (isLogin) {
			setLastPosition(getPosition());
		}
	} else if (isMapLoaded) {
		if (creature->getPosition().z == getPosition().z) {
			updateTileCache(creature->getTile(), creature->getPosition());
		}
	}
}

void Creature::onRemoveCreature(Creature* creature, bool)
{
	onCreatureDisappear(creature, true);
	if (creature == this) {
		if (master && !master->isRemoved()) {
			setMaster(nullptr);
		}
	} else if (isMapLoaded) {
		if (creature->getPosition().z == getPosition().z) {
			updateTileCache(creature->getTile(), creature->getPosition());
		}
	}
}


void Creature::onCreatureDisappear(const Creature* creature, bool isLogout)
{
	if (attackedCreature == creature) {
		setAttackedCreature(nullptr);
		onAttackedCreatureDisappear(isLogout);
	}

	if (followCreature == creature) {
		setFollowCreature(nullptr);
		onFollowCreatureDisappear(isLogout);
	}
}

void Creature::onChangeZone(ZoneType_t zone)
{
	if (attackedCreature && zone == ZONE_PROTECTION) {
		onCreatureDisappear(attackedCreature, false);
	}
}

void Creature::onAttackedCreatureChangeZone(ZoneType_t zone)
{
	if (zone == ZONE_PROTECTION) {
		onCreatureDisappear(attackedCreature, false);
	}
}

void Creature::onCreatureMove(Creature* creature, const Tile* newTile, const Position& newPos,
                              const Tile* oldTile, const Position& oldPos, bool teleport)
{
	if (creature == this) {
		lastStep = OTSYS_TIME();
		lastStepCost = 1;

		if (!teleport) {
			if (oldPos.z != newPos.z) {
				//floor change extra cost
				lastStepCost = 2;
			} else if (Position::getDistanceX(newPos, oldPos) >= 1 && Position::getDistanceY(newPos, oldPos) >= 1) {
				//diagonal extra cost
				lastStepCost = 2.2;
			}
		} else {
			stopEventWalk();
		}

		if (!summons.empty()) { 
			masterWalked = true;

			for (Creature* summon : summons) {
				if (summon->hasCondition(CONDITION_MOVING) && summon->listWalkDir.empty()) {
					summon->removeCondition(CONDITION_MOVING);
				}

				const Position& pos = summon->getPosition();
				Tile* destTile = g_game.map.getTile(newPos);

				if (Position::getDistanceZ(newPos, pos) > 0 || 
					(std::max<int32_t>(Position::getDistanceX(newPos, pos), Position::getDistanceX(newPos, pos)) > 10) || 
						(std::max<int32_t>(Position::getDistanceY(newPos, pos), Position::getDistanceY(newPos, pos)) > 6)) {
					g_game.map.moveCreature(*summon, *destTile);
					g_game.addMagicEffect(pos, CONST_ME_TELEPORT);
				}
			}
		}

		if (!guardians.empty()) { 
			for (Creature* guardian : guardians) {
				const Position& pos = guardian->getPosition();
				Tile* destTile = g_game.map.getTile(newPos);


				if (Position::getDistanceZ(newPos, pos) > 0 || 
					(std::max<int32_t>(Position::getDistanceX(newPos, pos), Position::getDistanceX(newPos, pos)) > 10) || 
						(std::max<int32_t>(Position::getDistanceY(newPos, pos), Position::getDistanceY(newPos, pos)) > 6) ||
							(std::max<int32_t>(Position::getDistanceX(newPos, pos), Position::getDistanceY(newPos, pos)) > 10)) {
					g_game.map.moveCreature(*guardian, *destTile);
					g_game.addMagicEffect(pos, CONST_ME_TELEPORT);
				}
			}
		}

		if (newTile->getZone() != oldTile->getZone()) {
			onChangeZone(getZone());
		}

		//update map cache
		if (isMapLoaded) {
			if (teleport || oldPos.z != newPos.z) {
				updateMapCache();
			} else {
				const Position& myPos = getPosition();

				if (oldPos.y > newPos.y) { //north
					//shift y south
					for (int32_t y = mapWalkHeight - 1; --y >= 0;) {
						memcpy(localMapCache[y + 1], localMapCache[y], sizeof(localMapCache[y]));
					}

					//update 0
					for (int32_t x = -maxWalkCacheWidth; x <= maxWalkCacheWidth; ++x) {
						Tile* cacheTile = g_game.map.getTile(myPos.getX() + x, myPos.getY() - maxWalkCacheHeight, myPos.z);
						updateTileCache(cacheTile, x, -maxWalkCacheHeight);
					}
				} else if (oldPos.y < newPos.y) { // south
					//shift y north
					for (int32_t y = 0; y <= mapWalkHeight - 2; ++y) {
						memcpy(localMapCache[y], localMapCache[y + 1], sizeof(localMapCache[y]));
					}

					//update mapWalkHeight - 1
					for (int32_t x = -maxWalkCacheWidth; x <= maxWalkCacheWidth; ++x) {
						Tile* cacheTile = g_game.map.getTile(myPos.getX() + x, myPos.getY() + maxWalkCacheHeight, myPos.z);
						updateTileCache(cacheTile, x, maxWalkCacheHeight);
					}
				}

				if (oldPos.x < newPos.x) { // east
					//shift y west
					int32_t starty = 0;
					int32_t endy = mapWalkHeight - 1;
					int32_t dy = Position::getDistanceY(oldPos, newPos);

					if (dy < 0) {
						endy += dy;
					} else if (dy > 0) {
						starty = dy;
					}

					for (int32_t y = starty; y <= endy; ++y) {
						for (int32_t x = 0; x <= mapWalkWidth - 2; ++x) {
							localMapCache[y][x] = localMapCache[y][x + 1];
						}
					}

					//update mapWalkWidth - 1
					for (int32_t y = -maxWalkCacheHeight; y <= maxWalkCacheHeight; ++y) {
						Tile* cacheTile = g_game.map.getTile(myPos.x + maxWalkCacheWidth, myPos.y + y, myPos.z);
						updateTileCache(cacheTile, maxWalkCacheWidth, y);
					}
				} else if (oldPos.x > newPos.x) { // west
					//shift y east
					int32_t starty = 0;
					int32_t endy = mapWalkHeight - 1;
					int32_t dy = Position::getDistanceY(oldPos, newPos);

					if (dy < 0) {
						endy += dy;
					} else if (dy > 0) {
						starty = dy;
					}

					for (int32_t y = starty; y <= endy; ++y) {
						for (int32_t x = mapWalkWidth - 1; --x >= 0;) {
							localMapCache[y][x + 1] = localMapCache[y][x];
						}
					}

					//update 0
					for (int32_t y = -maxWalkCacheHeight; y <= maxWalkCacheHeight; ++y) {
						Tile* cacheTile = g_game.map.getTile(myPos.x - maxWalkCacheWidth, myPos.y + y, myPos.z);
						updateTileCache(cacheTile, -maxWalkCacheWidth, y);
					}
				}

				updateTileCache(oldTile, oldPos);
			}
		}
	} else {
		if (isMapLoaded) {
			const Position& myPos = getPosition();

			if (newPos.z == myPos.z) {
				updateTileCache(newTile, newPos);
			}

			if (oldPos.z == myPos.z) {
				updateTileCache(oldTile, oldPos);
			}
		}
	}

	if (creature == followCreature || (creature == this && followCreature)) {
		if (hasFollowPath) {
           // if (getWalkDelay() <= 0) {
            //	g_dispatcher.addTask(createTask(std::bind(&Game::updateCreatureWalk, &g_game, getID()))); //// movimento modificado / melhorado!
          //  }
             //isUpdatingPath = true;
			 g_game.addToCheckFollow(this);
		}

		if (newPos.z != oldPos.z || !canSee(followCreature->getPosition())) {
			onCreatureDisappear(followCreature, false);
		}
	}

	if (creature == attackedCreature || (creature == this && attackedCreature)) {
		if (newPos.z != oldPos.z || !canSee(attackedCreature->getPosition())) {
			onCreatureDisappear(attackedCreature, false);
		} else {
			if (hasExtraSwing()) {
				//our target is moving lets see if we can get in hit
				g_dispatcher.addTask(createTask(std::bind(&Game::checkCreatureAttack, &g_game, getID())));
			}

			if (newTile->getZone() != oldTile->getZone()) {
				onAttackedCreatureChangeZone(attackedCreature->getZone());
			}
		}
	}
}

void Creature::onDeath()
{
	bool lastHitUnjustified = false;
	bool mostDamageUnjustified = false;
	Creature* lastHitCreature = g_game.getCreatureByID(lastHitCreatureId);
	Creature* lastHitCreatureMaster;
	if (lastHitCreature) {
		lastHitUnjustified = lastHitCreature->onKilledCreature(this);
		lastHitCreatureMaster = lastHitCreature->getMaster();
	} else {
		lastHitCreatureMaster = nullptr;
	}

	Creature* mostDamageCreature = nullptr;

	const int64_t timeNow = OTSYS_TIME();
	const uint32_t inFightTicks = g_config.getNumber(ConfigManager::PZ_LOCKED);
	int64_t mostDamage = 0;
	std::map<Creature*, uint64_t> experienceMap;
	for (const auto& it : damageMap) {
		if (Creature* attacker = g_game.getCreatureByID(it.first)) {
			CountBlock_t cb = it.second;
			if ((cb.total > mostDamage && (timeNow - cb.ticks <= inFightTicks))) {
				mostDamage = cb.total;
				mostDamageCreature = attacker;
			}

			if (attacker != this) {
				uint64_t gainExp = getGainedExperience(attacker);
				if (Player* attackerPlayer = attacker->getPlayer()) {
					attackerPlayer->removeAttacked(getPlayer());
					
					Party* party = attackerPlayer->getParty();
					if (party && party->getLeader() && party->isSharedExperienceActive() && party->isSharedExperienceEnabled()) {
						attacker = party->getLeader();
					}
				}

				auto tmpIt = experienceMap.find(attacker);
				if (tmpIt == experienceMap.end()) {
					experienceMap[attacker] = gainExp;
				} else {
					tmpIt->second += gainExp;
				}
			}
		}
	}

	for (const auto& it : experienceMap) {
		it.first->onGainExperience(it.second, this);
	}

	if (mostDamageCreature) {
		if (mostDamageCreature != lastHitCreature && mostDamageCreature != lastHitCreatureMaster) {
			Creature* mostDamageCreatureMaster = mostDamageCreature->getMaster();
			if (lastHitCreature != mostDamageCreatureMaster && (lastHitCreatureMaster == nullptr || mostDamageCreatureMaster != lastHitCreatureMaster)) {
				mostDamageUnjustified = mostDamageCreature->onKilledCreature(this, false);
			}
		}
	}

	bool droppedCorpse = dropCorpse(lastHitCreature, mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
	death(lastHitCreature);

	if (master) {
		setMaster(nullptr);
	}

	if (droppedCorpse) {
		g_game.removeCreature(this, false);
	}
}

bool Creature::dropCorpse(Creature* lastHitCreature, Creature* mostDamageCreature, bool lastHitUnjustified, bool mostDamageUnjustified)
{
	if (!lootDrop && getMonster()) {
		if (master) {
			//scripting event - onDeath
			const CreatureEventList& deathEvents = getCreatureEvents(CREATURE_EVENT_DEATH);
			for (CreatureEvent* deathEvent : deathEvents) {
				deathEvent->executeOnDeath(this, nullptr, lastHitCreature, mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
			}
		}

		g_game.addMagicEffect(getPosition(), CONST_ME_POFF);
	} else {
		Item* splash = nullptr;

		Tile* tile = getTile();
		Item* corpse = nullptr;
	
		if (splash) {
			g_game.internalAddItem(tile, splash, INDEX_WHEREEVER, FLAG_NOLIMIT);
			g_game.startDecay(splash);
		}

		corpse = getCorpse(lastHitCreature, mostDamageCreature);
		if (corpse) {
			g_game.internalAddItem(tile, corpse, INDEX_WHEREEVER, FLAG_NOLIMIT);
			g_game.startDecay(corpse);
		}

		//scripting event - onDeath
		for (CreatureEvent* deathEvent : getCreatureEvents(CREATURE_EVENT_DEATH)) {
			deathEvent->executeOnDeath(this, corpse, lastHitCreature, mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
		}

		if (corpse) {
			dropLoot(corpse->getContainer(), lastHitCreature);
		}

		//scripting event - onPostDeath
		for (CreatureEvent* postDeathEvent : getCreatureEvents(CREATURE_EVENT_POSTDEATH)) { //pota
			postDeathEvent->executeOnPostDeath(this, corpse, lastHitCreature, mostDamageCreature, lastHitUnjustified, mostDamageUnjustified);
		}
	}

	return true;
}

bool Creature::hasBeenAttacked(uint32_t attackerId)
{
	auto it = damageMap.find(attackerId);
	if (it == damageMap.end()) {
		return false;
	}
	return (OTSYS_TIME() - it->second.ticks) <= g_config.getNumber(ConfigManager::PZ_LOCKED);
}

Item* Creature::getCorpse(Creature*, Creature*)
{
	return Item::CreateItem(getLookCorpse());
}

void Creature::changeHealth(int64_t healthChange, bool sendHealthChange/* = true*/)
{
	int64_t oldHealth = health;

	if (healthChange > 0) {
		health += std::min<int64_t>(healthChange, getMaxHealth() - health);
	} else {
		health = std::max<int64_t>(0, health + healthChange);
	}

	if (sendHealthChange && oldHealth != health) {
		g_game.addCreatureHealth(this);
	}
	
	if (health <= 0) {
		g_dispatcher.addTask(createTask(std::bind(&Game::executeDeath, &g_game, getID())));
	}
}

void Creature::gainHealth(Creature* healer, int64_t healthGain)
{
	changeHealth(healthGain);
	if (healer) {
		healer->onTargetCreatureGainHealth(this, healthGain);
	}
}

void Creature::drainHealth(Creature* attacker, int64_t damage)
{
	changeHealth(-damage, false);

	if (attacker) {
		attacker->onAttackedCreatureDrainHealth(this, damage);
	}
}

BlockType_t Creature::blockHit(Creature* attacker, CombatType_t combatType, int64_t& damage,
                               bool checkDefense /* = false */, bool checkArmor /* = false */, bool /* field  = false */)
{
	BlockType_t blockType = BLOCK_NONE;

	if (isImmune(combatType)) {
		damage = 0;
		blockType = BLOCK_IMMUNITY;
	} else if (checkDefense || checkArmor) {
		bool hasDefense = false;

		if (blockCount > 0) {
			--blockCount;
			hasDefense = true;
		}

		if (checkDefense && hasDefense && canUseDefense) {
			int32_t defense = getDefense();
			damage -= uniform_random(defense / 2, defense);
			if (damage <= 0) {
				damage = 0;
				blockType = BLOCK_DEFENSE;
				checkArmor = false;
			}
		}

		if (checkArmor) {
			int32_t armor = getArmor();
			if (armor > 3) {
				damage -= uniform_random(armor / 2, armor - (armor % 2 + 1));
			} else if (armor > 0) {
				--damage;
			}

			if (damage <= 0) {
				damage = 0;
				blockType = BLOCK_ARMOR;
			}
		}

		if (hasDefense && blockType != BLOCK_NONE) {
			onBlockHit();
		}
	}

	if (attacker) {
		attacker->onAttackedCreature(this);
		attacker->onAttackedCreatureBlockHit(blockType);
	}

	onAttacked();
	return blockType;
}

bool Creature::setAttackedCreature(Creature* creature)
{
	if (creature) {
		const Position& creaturePos = creature->getPosition();
		if (creaturePos.z != getPosition().z || !canSee(creaturePos)) {
			attackedCreature = nullptr;
			return false;
		}

		attackedCreature = creature;
		onAttackedCreature(attackedCreature);
		attackedCreature->onAttacked();
	} else {
		attackedCreature = nullptr;
	}

	for (Creature* summon : summons) {
		summon->setAttackedCreature(creature);
	}
	return true;
}

void Creature::getPathSearchParams(const Creature*, FindPathParams& fpp) const
{
	fpp.fullPathSearch = !hasFollowPath;
	fpp.clearSight = true;
	fpp.maxSearchDist = 12;
	fpp.minTargetDist = 1;
	fpp.maxTargetDist = 1;
}


void Creature::goToFollowCreature()
{
	if (followCreature) {
		FindPathParams fpp;
		getPathSearchParams(followCreature, fpp);

		Monster* monster = getMonster();
		if (monster && !monster->getMaster() && (monster->isFleeing() || fpp.maxTargetDist > 1)) {
			Direction dir = DIRECTION_NONE;

			if (monster->isFleeing()) {
				monster->getDistanceStep(followCreature->getPosition(), dir, true);
			} else { //maxTargetDist > 1
				if (!monster->getDistanceStep(followCreature->getPosition(), dir)) {
					// if we can't get anything then let the A* calculate
					listWalkDir.clear();
					if (getPathTo(followCreature->getPosition(), listWalkDir, fpp)) {
						hasFollowPath = true;
						//startAutoWalk(listWalkDir);
					} else {
						hasFollowPath = false;
					}
					return;
				}
			}

			if (dir != DIRECTION_NONE) {
				listWalkDir.clear();
				listWalkDir.push_front(dir);

				hasFollowPath = true;
				//startAutoWalk(listWalkDir);
			}
		} else {
			
			if ((getCondition(ConditionType_t(CONDITION_MOVING)) && !masterWalked)) {
				return; 
			}
			
			listWalkDir.clear();
			if (getPathTo(followCreature->getPosition(), listWalkDir, fpp)) {
				hasFollowPath = true;
				//startAutoWalk(listWalkDir);
			} else {
				hasFollowPath = false;
			}
		}
	}
}

void Creature::goToFollowCreatureContinue()
{
	if (hasFollowPath)
		startAutoWalk(listWalkDir);
	onFollowCreatureComplete(followCreature);
}

bool Creature::setFollowCreature(Creature* creature)
{
	if (creature) {
		if (followCreature == creature) {
			return true;
		}

		const Position& creaturePos = creature->getPosition();
		if (creaturePos.z != getPosition().z || !canSee(creaturePos)) {
			followCreature = nullptr;
			return false;
		}

		if (!listWalkDir.empty()) {
			listWalkDir.clear();
			onWalkAborted();
		}

		hasFollowPath = false;
		forceUpdateFollowPath = false;
		followCreature = creature;
		isUpdatingPath = true;
	} else {
		isUpdatingPath = false;
		followCreature = nullptr;
	}

	onFollowCreature(creature);
	return true;
}

double Creature::getDamageRatio(Creature* attacker) const
{
	uint32_t totalDamage = 0;
	uint64_t attackerDamage = 0;

	for (const auto& it : damageMap) {
		const CountBlock_t& cb = it.second;
		totalDamage += cb.total;
		if (it.first == attacker->getID()) {
			attackerDamage += cb.total;
		}
	}

	if (totalDamage == 0) {
		return 0;
	}

	return (static_cast<double>(attackerDamage) / totalDamage);
}

uint64_t Creature::getGainedExperience(Creature* attacker) const
{
	return std::floor(getDamageRatio(attacker) * getLostExperience());
}

void Creature::addDamagePoints(Creature* attacker, int64_t damagePoints)
{
	if (damagePoints <= 0) {
		return;
	}

	uint32_t attackerId = attacker->id;

	if (attacker->isSummon()) { //pota		
		Creature* master = attacker->getMaster();
		if (Player* player = master->getPlayer()) {
			attackerId = player->id;
		}
	}

	auto it = damageMap.find(attackerId);
	if (it == damageMap.end()) {
		CountBlock_t cb;
		cb.ticks = OTSYS_TIME();
		cb.total = damagePoints;
		damageMap[attackerId] = cb;
	} else {
		it->second.total += damagePoints;
		it->second.ticks = OTSYS_TIME();
	}

	lastHitCreatureId = attackerId;
}

void Creature::onAddCondition(ConditionType_t type)
{
	if (type == CONDITION_PARALYZE && hasCondition(CONDITION_HASTE)) {
		removeCondition(CONDITION_HASTE);
	} else if (type == CONDITION_HASTE && hasCondition(CONDITION_PARALYZE)) {
		removeCondition(CONDITION_PARALYZE);
	}
	
}

void Creature::onAddCombatCondition(ConditionType_t)
{
	//
}

void Creature::onEndCondition(ConditionType_t)
{
	//
}

void Creature::onTickCondition(ConditionType_t type, bool& bRemove)
{
	const MagicField* field = getTile()->getFieldItem();
	if (!field) {
		return;
	}

	switch (type) {
		case CONDITION_FIRE:
			bRemove = (field->getCombatType() != COMBAT_FIREDAMAGE);
			break;
		case CONDITION_ENERGY:
			bRemove = (field->getCombatType() != COMBAT_ENERGYDAMAGE);
			break;
		case CONDITION_POISON:
			bRemove = (field->getCombatType() != COMBAT_POISONDAMAGE);
			break;
		case CONDITION_FREEZING:
			bRemove = (field->getCombatType() != COMBAT_ICEDAMAGE);
			break;
		case CONDITION_DAZZLED:
			bRemove = (field->getCombatType() != COMBAT_HOLYDAMAGE);
			break;
		case CONDITION_CURSED:
			bRemove = (field->getCombatType() != COMBAT_DEATHDAMAGE);
			break;
		case CONDITION_DROWN:
			bRemove = (field->getCombatType() != COMBAT_DROWNDAMAGE);
			break;
		case CONDITION_BLEEDING:
			bRemove = (field->getCombatType() != COMBAT_PHYSICALDAMAGE);
			break;
		default:
			break;
	}
}

void Creature::onCombatRemoveCondition(Condition* condition)
{
	removeCondition(condition);
}

void Creature::onAttacked()
{
	//
}

void Creature::onAttackedCreatureDrainHealth(Creature* target, int64_t points)
{
	target->addDamagePoints(this, points);
}

bool Creature::onKilledCreature(Creature* target, bool)
{
	if (master) {
		master->onKilledCreature(target);
	}

	//scripting event - onKill
	const CreatureEventList& killEvents = getCreatureEvents(CREATURE_EVENT_KILL);
	for (CreatureEvent* killEvent : killEvents) {
		killEvent->executeOnKill(this, target);
	}
	return false;
}

void Creature::onGainExperience(uint64_t gainExp, Creature* target)
{
	if (gainExp == 0 || !master) {
		return;
	}

	//do not give exp to summons //pota
	//gainExp /= 2; 

	master->onGainExperience(gainExp, target);

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, position, false, true);
	if (spectators.empty()) {
		return;
	}
	//do not give exp to summons //pota
	if (this->getMonster() || master->getMonster()) {
		//master->onGainExperience(gainExp, target);
        	return;
    	}

	TextMessage message(MESSAGE_EXPERIENCE_OTHERS, ucfirst(getNameDescription()) + " gained " + std::to_string(gainExp) + (gainExp != 1 ? " experience points." : " experience point."));
	message.position = position;
	message.primary.color = TEXTCOLOR_WHITE_EXP;
	message.primary.value = gainExp;

	for (Creature* spectator : spectators) {
		spectator->getPlayer()->sendTextMessage(message);
	}
}

bool Creature::setMaster(Creature* newMaster) {
	if (!newMaster && !master) {
		return false;
	}

	if (newMaster) {
		incrementReferenceCounter();
		newMaster->summons.push_back(this);
	}

	if (master) {
		auto summon = std::find(master->summons.begin(), master->summons.end(), this);
		if (summon != master->summons.end()) {
			decrementReferenceCounter();
			master->summons.erase(summon);
		}
	}
	master = newMaster;
	return true;
}

bool Creature::addCondition(Condition* condition, bool force/* = false*/)
{
	if (condition == nullptr) {
		return false;
	}

	if (!force && condition->getType() == CONDITION_HASTE && hasCondition(CONDITION_PARALYZE)) {
		int64_t walkDelay = getWalkDelay();
		if (walkDelay > 0) {
			g_scheduler.addEvent(createSchedulerTask(walkDelay, std::bind(&Game::forceAddCondition, &g_game, getID(), condition)));
			return false;
		}
	}

	Condition* prevCond = getCondition(condition->getType(), condition->getId(), condition->getSubId());
	if (prevCond) {
		prevCond->addCondition(this, condition);
		delete condition;
		return true;
	}

	if (condition->startCondition(this)) {
		conditions.push_back(condition);
		onAddCondition(condition->getType());
		return true;
	}

	delete condition;
	return false;
}

bool Creature::addCombatCondition(Condition* condition)
{
	//Caution: condition variable could be deleted after the call to addCondition
	ConditionType_t type = condition->getType();

	if (!addCondition(condition)) {
		return false;
	}

	onAddCombatCondition(type);
	return true;
}

void Creature::removeCondition(ConditionType_t type, bool force/* = false*/)
{
	auto it = conditions.begin(), end = conditions.end();
	while (it != end) {
		Condition* condition = *it;
		if (condition->getType() != type) {
			++it;
			continue;
		}

		if (!force && type == CONDITION_PARALYZE) {
			int64_t walkDelay = getWalkDelay();
			if (walkDelay > 0) {
				g_scheduler.addEvent(createSchedulerTask(walkDelay, std::bind(&Game::forceRemoveCondition, &g_game, getID(), type)));
				return;
			}
		}

		it = conditions.erase(it);

		condition->endCondition(this);
		delete condition;

		onEndCondition(type);
	}
}

void Creature::removeCondition(ConditionType_t type, ConditionId_t conditionId, bool force/* = false*/)
{
	auto it = conditions.begin(), end = conditions.end();
	while (it != end) {
		Condition* condition = *it;
		if (condition->getType() != type || condition->getId() != conditionId) {
			++it;
			continue;
		}

		if (!force && type == CONDITION_PARALYZE) {
			int64_t walkDelay = getWalkDelay();
			if (walkDelay > 0) {
				g_scheduler.addEvent(createSchedulerTask(walkDelay, std::bind(&Game::forceRemoveCondition, &g_game, getID(), type)));
				return;
			}
		}

		it = conditions.erase(it);

		condition->endCondition(this);
		delete condition;

		onEndCondition(type);
	}
}

void Creature::removeCombatCondition(ConditionType_t type)
{
	std::vector<Condition*> removeConditions;
	for (Condition* condition : conditions) {
		if (condition->getType() == type) {
			removeConditions.push_back(condition);
		}
	}

	for (Condition* condition : removeConditions) {
		onCombatRemoveCondition(condition);
	}
}

void Creature::removeCondition(Condition* condition, bool force/* = false*/)
{
	auto it = std::find(conditions.begin(), conditions.end(), condition);
	if (it == conditions.end()) {
		return;
	}

	if (!force && condition->getType() == CONDITION_PARALYZE) {
		int64_t walkDelay = getWalkDelay();
		if (walkDelay > 0) {
			g_scheduler.addEvent(createSchedulerTask(walkDelay, std::bind(&Game::forceRemoveCondition, &g_game, getID(), condition->getType())));
			return;
		}
	}

	conditions.erase(it);

	condition->endCondition(this);
	onEndCondition(condition->getType());
	delete condition;
}

Condition* Creature::getCondition(ConditionType_t type) const
{
	for (Condition* condition : conditions) {
		if (condition->getType() == type) {
			return condition;
		}
	}
	return nullptr;
}

Condition* Creature::getCondition(ConditionType_t type, ConditionId_t conditionId, uint32_t subId/* = 0*/) const
{
	for (Condition* condition : conditions) {
		if (condition->getType() == type && condition->getId() == conditionId && condition->getSubId() == subId) {
			return condition;
		}
	}
	return nullptr;
}

void Creature::executeConditions(uint32_t interval)
{
	ConditionList tempConditions{ conditions };
	for (Condition* condition : tempConditions) {
		auto it = std::find(conditions.begin(), conditions.end(), condition);
		if (it == conditions.end()) {
			continue;
		}

		if (!condition->executeCondition(this, interval)) {
			it = std::find(conditions.begin(), conditions.end(), condition);
			if (it != conditions.end()) {
				conditions.erase(it);
				condition->endCondition(this);
				onEndCondition(condition->getType());
				delete condition;
			}
		}
	}
}
bool Creature::hasCondition(ConditionType_t type, uint32_t subId/* = 0*/) const
{
	if (isSuppress(type)) {
		return false;
	}

	int64_t timeNow = OTSYS_TIME();
	for (Condition* condition : conditions) {
		if (condition->getType() != type || condition->getSubId() != subId) {
			continue;
		}

		if (condition->getEndTime() >= timeNow || condition->getTicks() == -1) {
			return true;
		}
	}
	return false;
}

bool Creature::isImmuneEvent(CombatType_t combatType, ConditionType_t conditionType)
{
	if (getMonster() && combatType != COMBAT_NONE) {
		if (g_events->eventMonsterOnCheckImmunity(getMonster(), combatType, conditionType)) {
			return true;
		}
	}

	return hasBitSet(static_cast<uint32_t>(combatType), getDamageImmunities());
}

bool Creature::isImmune(CombatType_t type) const
{
	return hasBitSet(static_cast<uint32_t>(type), getDamageImmunities());
}

bool Creature::isImmune(ConditionType_t type) const
{
	return hasBitSet(static_cast<uint32_t>(type), getConditionImmunities());
}

bool Creature::isSuppress(ConditionType_t type) const
{
	return hasBitSet(static_cast<uint32_t>(type), getConditionSuppressions());
}

int64_t Creature::getStepDuration(Direction dir) const
{
	int64_t stepDuration = getStepDuration();
	if ((dir & DIRECTION_DIAGONAL_MASK) != 0) {
		stepDuration *= 3;
	}
	return stepDuration;
}

int64_t Creature::getStepDuration() const
{
	if (isRemoved()) {
		return 0;
	}

	uint32_t calculatedStepSpeed;
	uint32_t groundSpeed;

	int32_t stepSpeed = getStepSpeed();
	if (stepSpeed > -Creature::speedB) {
		calculatedStepSpeed = floor((Creature::speedA * log((stepSpeed / 2) + Creature::speedB) + Creature::speedC) + 0.5);
		if (calculatedStepSpeed == 0) {
			calculatedStepSpeed = 1;
		}
	} else {
		calculatedStepSpeed = 1;
	}

	Item* ground = tile->getGround();
	if (ground) {
		groundSpeed = Item::items[ground->getID()].speed;
		if (groundSpeed == 0) {
			groundSpeed = 150;
		}
	} else {
		groundSpeed = 150;
	}

	double duration = std::floor(1000 * groundSpeed / calculatedStepSpeed);
	int64_t stepDuration = std::ceil(duration / 50) * 50;

	const Monster* monster = getMonster();
	if (monster && monster->isTargetNearby() && !monster->isFleeing() && !monster->getMaster()) {
		stepDuration *= 2;
	}

	return stepDuration;
}

int64_t Creature::getEventStepTicks(bool onlyDelay) const
{
	int64_t ret = getWalkDelay();
	if (ret <= 0) {
		int64_t stepDuration = getStepDuration();
		if (onlyDelay && stepDuration > 0) {
			ret = 1;
		} else {
			ret = stepDuration * lastStepCost;
		}
	}
	return ret;
}

LightInfo Creature::getCreatureLight() const
{
	return internalLight;
}

void Creature::setCreatureLight(LightInfo lightInfo) {
	internalLight = std::move(lightInfo);
}

void Creature::setNormalCreatureLight()
{
	
internalLight = {};

}

bool Creature::registerCreatureEvent(const std::string& name)
{
	CreatureEvent* event = g_creatureEvents->getEventByName(name);
	if (!event) {
		return false;
	}

	CreatureEventType_t type = event->getEventType();
	if (hasEventRegistered(type)) {
		for (CreatureEvent* creatureEvent : eventsList) {
			if (!creatureEvent->isLoaded()) {
				continue;
			}
			if (creatureEvent == event) {
				return false;
			}
		}
	} else {
		scriptEventsBitField |= static_cast<uint32_t>(1) << type;
	}

	eventsList.push_back(event);
	return true;
}

bool Creature::unregisterCreatureEvent(const std::string& name)
{
	CreatureEvent* event = g_creatureEvents->getEventByName(name);
	if (!event) {
		return false;
	}

	CreatureEventType_t type = event->getEventType();
	if (!hasEventRegistered(type)) {
		return false;
	}

	bool resetTypeBit = true;

	auto it = eventsList.begin(), end = eventsList.end();
	while (it != end) {
		CreatureEvent* curEvent = *it;
		if (curEvent == event) {
			it = eventsList.erase(it);
			continue;
		}

		if (curEvent->getEventType() == type) {
			resetTypeBit = false;
		}
		++it;
	}

	if (resetTypeBit) {
		scriptEventsBitField &= ~(static_cast<uint32_t>(1) << type);
	}
	return true;
}

CreatureEventList Creature::getCreatureEvents(CreatureEventType_t type)
{
	CreatureEventList tmpEventList;

	if (!hasEventRegistered(type)) {
		return tmpEventList;
	}

	for (CreatureEvent* creatureEvent : eventsList) {
		if (creatureEvent->getEventType() == type) {
			tmpEventList.push_back(creatureEvent);
		}
	}

	return tmpEventList;
}

bool FrozenPathingConditionCall::isInRange(const Position& startPos, const Position& testPos,
        const FindPathParams& fpp) const
{
	if (fpp.fullPathSearch) {
		if (testPos.x > targetPos.x + fpp.maxTargetDist) {
			return false;
		}

		if (testPos.x < targetPos.x - fpp.maxTargetDist) {
			return false;
		}

		if (testPos.y > targetPos.y + fpp.maxTargetDist) {
			return false;
		}

		if (testPos.y < targetPos.y - fpp.maxTargetDist) {
			return false;
		}
	} else {
		int_fast32_t dx = Position::getOffsetX(startPos, targetPos);

		int32_t dxMax = (dx >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x > targetPos.x + dxMax) {
			return false;
		}

		int32_t dxMin = (dx <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.x < targetPos.x - dxMin) {
			return false;
		}

		int_fast32_t dy = Position::getOffsetY(startPos, targetPos);

		int32_t dyMax = (dy >= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y > targetPos.y + dyMax) {
			return false;
		}

		int32_t dyMin = (dy <= 0 ? fpp.maxTargetDist : 0);
		if (testPos.y < targetPos.y - dyMin) {
			return false;
		}
	}
	return true;
}

bool FrozenPathingConditionCall::operator()(const Position& startPos, const Position& testPos,
        const FindPathParams& fpp, int32_t& bestMatchDist) const
{
	if (!isInRange(startPos, testPos, fpp)) {
		return false;
	}

	if (fpp.clearSight && !g_game.isSightClear(testPos, targetPos, true)) {
		return false;
	}

	int32_t testDist = std::max<int32_t>(Position::getDistanceX(targetPos, testPos), Position::getDistanceY(targetPos, testPos));
	if (fpp.maxTargetDist == 1) {
		if (testDist < fpp.minTargetDist || testDist > fpp.maxTargetDist) {
			return false;
		}

		return true;
	} else if (testDist <= fpp.maxTargetDist) {
		if (testDist < fpp.minTargetDist) {
			return false;
		}

		if (testDist == fpp.maxTargetDist) {
			bestMatchDist = 0;
			return true;
		} else if (testDist > bestMatchDist) {
			//not quite what we want, but the best so far
			bestMatchDist = testDist;
			return true;
		}
	}
	return false;
}

bool Creature::isInvisible() const
{
	return std::find_if(conditions.begin(), conditions.end(), [] (const Condition* condition) {
		return condition->getType() == CONDITION_INVISIBLE;
	}) != conditions.end();
}

void Creature::insertIcon(const std::string& iconName, int16_t pointX, int16_t pointY) {
    this->creatureIconMap[iconName] = std::make_pair(pointX, pointY);
	refreshTile();
}

void Creature::removeIcon(const std::string& iconName) {
    auto it = this->creatureIconMap.find(iconName);
    if (it != this->creatureIconMap.end()) {
        this->creatureIconMap.erase(it);
    }
	refreshTile();
}

void Creature::cleanIcons()
{
    std::vector<std::string> iconsToRemove;

    for (auto it = creatureIconMap.begin(); it != creatureIconMap.end(); ++it) {
        const std::string& iconName = it->first;
        iconsToRemove.push_back(iconName);
    }

    for (const std::string& iconName : iconsToRemove) {
        removeIcon(iconName);
    }
}

void Creature::insertAttachedEffect(uint16_t effectId, uint8_t front) 
{
	attachedEffects[effectId] = front;
	refreshTile();
}

void Creature::removeAttachedEffect(uint16_t effectId) 
{
	attachedEffects.erase(effectId);
	refreshTile();
}

void Creature::cleanAttachedEffects()
{
	std::vector<uint16_t> effectsToRemove;

	for (auto it = attachedEffects.begin(); it != attachedEffects.end(); ++it) {
		const uint16_t effectId = it->first;
		effectsToRemove.push_back(effectId);
	}

	for (const uint16_t effectId : effectsToRemove) {
		removeAttachedEffect(effectId);
	}
}

const std::map<std::string, std::pair<int16_t, int16_t>>& Creature::getIconMap() const {
    return this->creatureIconMap;
}

void Creature::refreshTile()
{
	const Position creaturePos = getPosition();
	Tile* tile = g_game.map.getTile(creaturePos);
	if (tile) {
		SpectatorVec spectators;
		g_game.map.getSpectators(spectators, creaturePos, true, true);
		for (Creature* spectator : spectators) {
			if(spectator->getPlayer()) {
				spectator->getPlayer()->sendUpdateTile(tile, creaturePos);
			}
		}
	}
}

bool Creature::getPathTo(const Position& targetPos, std::forward_list<Direction>& dirList, const FindPathParams& fpp) const
{
	return g_game.map.getPathMatching(*this, dirList, FrozenPathingConditionCall(targetPos), fpp);
}

bool Creature::getPathTo(const Position& targetPos, std::forward_list<Direction>& dirList, int32_t minTargetDist, int32_t maxTargetDist, bool fullPathSearch /*= true*/, bool clearSight /*= true*/, int32_t maxSearchDist /*= 0*/) const
{
	FindPathParams fpp;
	fpp.fullPathSearch = fullPathSearch;
	fpp.maxSearchDist = maxSearchDist;
	fpp.clearSight = clearSight;
	fpp.minTargetDist = minTargetDist;
	fpp.maxTargetDist = maxTargetDist;
	return getPathTo(targetPos, dirList, fpp);
}

void Creature::addStringStorageValue(const uint32_t key, const std::string value)
{
	if (!value.empty()) {
		stringStorageMap[key] = value;
	} else {
		stringStorageMap.erase(key);
	}
}

bool Creature::getStringStorageValue(const uint32_t key, std::string& value) const
{
	auto it = stringStorageMap.find(key);
	if (it == stringStorageMap.end()) {
		value = -1;
		return false;
	}

	value = it->second;
	return true;
}

void Creature::addGuardian(Creature* guardian)
{
	guardian->setDropLoot(false);
	guardian->setSkillLoss(false);
	guardian->setMaster(this);
	guardian->incrementReferenceCounter();
	guardian->setIsGuardian(true);
	guardians.push_back(guardian);
}

void Creature::removeGuardian(Creature* guardian)
{
	auto cit = std::find(guardians.begin(), guardians.end(), guardian);
	if (cit != guardians.end()) {
		guardian->setDropLoot(false);
		guardian->setSkillLoss(true);
		guardian->setMaster(nullptr);
		guardian->decrementReferenceCounter();
		guardians.erase(cit);
	}
}
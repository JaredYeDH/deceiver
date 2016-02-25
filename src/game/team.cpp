#include "team.h"
#include "game.h"
#include "minion.h"
#include "data/components.h"
#include "entities.h"
#include "data/animator.h"
#include "asset/animation.h"

#if DEBUG
#define PLAYER_SPAWN_DELAY 1.0f
#define MINION_SPAWN_INITIAL_DELAY 2.0f
#else
#define PLAYER_SPAWN_DELAY 5.0f
#define MINION_SPAWN_INITIAL_DELAY 10.0f
#endif
#define MINION_SPAWN_INTERVAL 30.0f
#define MINION_SPAWN_GROUP_INTERVAL 2.0f

#define CREDITS_INITIAL 0

namespace VI
{


const Vec4 Team::colors[(s32)AI::Team::count] =
{
	Vec4(0.9f, 0.8f, 0.3f, 1),
	Vec4(0.8f, 0.3f, 0.3f, 1),
};

StaticArray<Team, (s32)AI::Team::count> Team::list;

Team::Team()
	: minion_spawn_timer(MINION_SPAWN_INITIAL_DELAY)
{
}

void Team::awake()
{
	for (s32 i = 0; i < targets.length; i++)
	{
		Target* t = targets[i].ref();
		if (t)
			t->target_hit.link<Team, const TargetEvent&, &Team::target_hit>(this);
	}
}

#define TARGET_CREDITS 50
void Team::target_hit(const TargetEvent& e)
{
	AI::Team other = e.hit_by->get<AIAgent>()->team;
	if (other != team())
	{
		e.hit_by->get<PlayerCommon>()->manager.ref()->add_credits(TARGET_CREDITS);
		World::remove_deferred(e.target);
		Team::list[(s32)other].score++;
	}
}

void Team::player_killed_by(Entity* e)
{
	if (e->has<Projectile>())
		e = e->get<Projectile>()->owner.ref();
	AI::Team other = e->get<AIAgent>()->team;
	if (other != team())
		Team::list[(s32)other].score++;
}

r32 minion_spawn_delay(Team::MinionSpawnState state)
{
	if (state == Team::MinionSpawnState::One)
		return MINION_SPAWN_INTERVAL;
	else
		return MINION_SPAWN_GROUP_INTERVAL;
}

void Team::update(const Update& u)
{
	if (minion_spawns.length > 0)
	{
		minion_spawn_timer -= u.time.delta;
		if (minion_spawn_timer < 0)
		{
			for (int i = 0; i < minion_spawns.length; i++)
			{
				Vec3 pos;
				Quat rot;
				minion_spawns[i].ref()->absolute(&pos, &rot);
				pos += rot * Vec3(0, 0, MINION_SPAWN_RADIUS); // spawn it around the edges
				Entity* entity = World::create<Minion>(pos, rot, team());
			}
			minion_spawn_state = (MinionSpawnState)(((s32)minion_spawn_state + 1) % (s32)MinionSpawnState::count);
			minion_spawn_timer = minion_spawn_delay(minion_spawn_state);
		}
	}
}

u16 AbilitySlot::upgrade_costs[][ABILITY_LEVELS] =
{
	{ 10, 30, 50 }, // Stun
	{ 10, 30, 50 }, // Heal
	{ 10, 30, 50 }, // Stealth
	{ 10, 30, 50 }, // Turret
	{ 10, 30, 50 }, // Gun
};

const char* AbilitySlot::names[] =
{
	"Stun",
	"Heal",
	"Stealth",
	"Turret",
	"Gun",
};

b8 AbilitySlot::can_upgrade() const
{
	return level < ABILITY_LEVELS;
}

u16 AbilitySlot::upgrade_cost() const
{
	if (!can_upgrade())
		return UINT16_MAX;
	if (ability == Ability::None)
	{
		// return cheapest possible upgrade
		u16 cheapest_upgrade = UINT16_MAX;
		for (s32 i = 0; i < (s32)Ability::count; i++)
		{
			if (upgrade_costs[i][0] < cheapest_upgrade)
				cheapest_upgrade = upgrade_costs[i][0];
		}
		return cheapest_upgrade;
	}
	else
		return upgrade_costs[(s32)ability][level];
}

PinArray<PlayerManager, MAX_PLAYERS> PlayerManager::list;

PlayerManager::PlayerManager(Team* team)
	: spawn_timer(PLAYER_SPAWN_DELAY),
	team(team),
	credits(CREDITS_INITIAL),
	abilities{ { Ability::None, 0 }, { Ability::None, 0 } }
{
}

void PlayerManager::upgrade(Ability ability)
{
	b8 upgraded = false;
	for (s32 i = 0; i < ABILITY_COUNT; i++)
	{
		if (abilities[i].can_upgrade())
		{
			if (abilities[i].ability == ability)
			{
				credits -= abilities[i].upgrade_cost();
				abilities[i].level++;
				upgraded = true;
				break;
			}
			else if (abilities[i].ability == Ability::None)
			{
				abilities[i].ability = ability;
				credits -= abilities[i].upgrade_cost();
				abilities[i].level = 1;
				upgraded = true;
				break;
			}
		}
	}
	vi_assert(upgraded); // check for invalid upgrade request
}

void PlayerManager::add_credits(u16 c)
{
	credits += c;
}

b8 PlayerManager::upgrade_available() const
{
	for (s32 i = 0; i < ABILITY_COUNT; i++)
	{
		if (abilities[i].level < ABILITY_LEVELS)
		{
			if (credits >= abilities[i].upgrade_cost())
				return true;
		}
	}
	return false;
}

void PlayerManager::update(const Update& u)
{
	if (team.ref()->player_spawn.ref() && !entity.ref())
	{
		spawn_timer -= u.time.delta;
		if (spawn_timer < 0 || Game::data.mode == Game::Mode::Parkour)
		{
			spawn.fire();
			spawn_timer = PLAYER_SPAWN_DELAY;
		}
	}
}


}
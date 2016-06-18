#include "scripts.h"
#include "entities.h"
#include "common.h"
#include "game.h"
#include "player.h"
#include "awk.h"
#include "audio.h"
#include "asset/Wwise_IDs.h"
#include "mersenne/mersenne-twister.h"
#include "asset/level.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "asset/shader.h"
#include "asset/dialogue.h"
#include "asset/font.h"
#include "asset/lookup.h"
#include "menu.h"
#include "render/views.h"
#include "cjson/cJSON.h"
#include "strings.h"
#include "utf8/utf8.h"
#include "ai_player.h"
#include "sha1/sha1.h"
#include "settings.h"
#include "utf8/utf8.h"
#include "console.h"
#include <unordered_map>
#include <string>

namespace VI
{

Script* Script::find(const char* name)
{
	s32 i = 0;
	while (true)
	{
		if (!Script::all[i].name)
			break;

		if (utf8cmp(Script::all[i].name, name) == 0)
			return &Script::all[i];

		i++;
	}
	return nullptr;
}

#define MAX_NODES 4096
#define MAX_BRANCHES 8
#define MAX_CHOICES 4
namespace Penelope
{
	enum class Face
	{
		Default,
		Sad,
		Upbeat,
		Urgent,
		EyesClosed,
		Smile,
		Wat,
		Unamused,
		Angry,
		Concerned,
		count,
	};

	enum class Matchmake
	{
		None,
		Searching,
		Found,
	};

	struct Node
	{
		static Node list[MAX_NODES];
		enum class Type
		{
			Node,
			Text,
			Choice,
			Branch,
			Set,
		};

		struct Branch
		{
			AssetID value;
			ID target;
		};

		struct Text
		{
			Face face;
			ID choices[MAX_CHOICES];
			AkUniqueID sound;
		};

		struct BranchData
		{
			AssetID variable;
			Branch branches[MAX_BRANCHES];
		};

		struct Set
		{
			AssetID variable;
			AssetID value;
		};

		Type type;
		AssetID name;
		ID next;
		union
		{
			Text text;
			Set set;
			BranchData branch;
		};
	};

	Node Node::list[MAX_NODES];

	// map string IDs to node IDs
	// note: multiple nodes may use the same string ID
	ID node_lookup[(s32)strings::count];

	void global_init()
	{
		Loader::font_permanent(Asset::Font::pt_sans);

		Array<cJSON*> trees;
		std::unordered_map<std::string, ID> id_lookup;

		// load dialogue trees and build node ID lookup table
		{
			ID current_node_id = 0;
			for (s32 i = 0; i < Asset::DialogueTree::count; i++)
			{
				cJSON* tree = Loader::dialogue_tree(i);
				trees.add(tree);

				cJSON* json_node = trees[i]->child;

				while (json_node)
				{
					vi_assert(current_node_id < MAX_NODES);
					const char* id = Json::get_string(json_node, "id");
					id_lookup[id] = current_node_id;
					current_node_id++;
					json_node = json_node->next;
				}
			}
		}

		// parse nodes

		ID current_node_id = 0;

		for (s32 tree_index = 0; tree_index < Asset::DialogueTree::count; tree_index++)
		{
			cJSON* json_node = trees[tree_index]->child;

			while (json_node)
			{
				Node* node = &Node::list[current_node_id];

				// type
				{
					const char* type = Json::get_string(json_node, "type");
					if (utf8cmp(type, "Node") == 0)
						node->type = Node::Type::Node;
					else if (utf8cmp(type, "Text") == 0)
						node->type = Node::Type::Text;
					else if (utf8cmp(type, "Choice") == 0)
						node->type = Node::Type::Choice;
					else if (utf8cmp(type, "Branch") == 0)
						node->type = Node::Type::Branch;
					else if (utf8cmp(type, "Set") == 0)
						node->type = Node::Type::Set;
					else
						vi_assert(false);
				}

				// name
				const char* name_str = Json::get_string(json_node, "name");
				char hash[41];

				{
					if (node->type == Node::Type::Text || node->type == Node::Type::Choice)
					{
						sha1::hash(name_str, hash);
						name_str = hash;
					}

					node->name = strings_get(name_str);
					if (node->name != AssetNull && node->name < (s32)strings::count)
						node_lookup[node->name] = current_node_id;
				}

				// next
				{
					const char* next = Json::get_string(json_node, "next");
					if (next)
						node->next = id_lookup[next];
					else
						node->next = IDNull;
				}

				if (node->type == Node::Type::Text)
				{
					const char* face = Json::get_string(json_node, "face");
					if (face)
					{
						if (utf8cmp(face, "Sad") == 0)
							node->text.face = Face::Sad;
						else if (utf8cmp(face, "Upbeat") == 0)
							node->text.face = Face::Upbeat;
						else if (utf8cmp(face, "Urgent") == 0)
							node->text.face = Face::Urgent;
						else if (utf8cmp(face, "EyesClosed") == 0)
							node->text.face = Face::EyesClosed;
						else if (utf8cmp(face, "Smile") == 0)
							node->text.face = Face::Smile;
						else if (utf8cmp(face, "Wat") == 0)
							node->text.face = Face::Wat;
						else if (utf8cmp(face, "Unamused") == 0)
							node->text.face = Face::Unamused;
						else if (utf8cmp(face, "Angry") == 0)
							node->text.face = Face::Angry;
						else if (utf8cmp(face, "Concerned") == 0)
							node->text.face = Face::Concerned;
						else
							node->text.face = Face::Default;
					}
					else
						node->text.face = Face::Default;

					// choices
					{
						for (s32 i = 0; i < MAX_CHOICES; i++)
							node->text.choices[i] = IDNull;

						cJSON* json_choices = cJSON_GetObjectItem(json_node, "choices");
						if (json_choices)
						{
							s32 i = 0;
							cJSON* json_choice = json_choices->child;
							while (json_choice)
							{
								vi_assert(i < MAX_CHOICES);
								node->text.choices[i] = id_lookup[json_choice->valuestring];
								i++;
								json_choice = json_choice->next;
							}
						}
					}

					// sound
					if (name_str)
					{
						char event_name[512];
						sprintf(event_name, "Play_%s", name_str);
						node->text.sound = Audio::get_id(event_name);
					}
					else
						node->text.sound = AK_INVALID_UNIQUE_ID;
				}

				if (node->type == Node::Type::Branch || node->type == Node::Type::Set)
				{
					const char* variable = Json::get_string(json_node, "variable");
					if (variable)
					{
						node->set.variable = strings_get(variable);
						vi_assert(node->set.variable != AssetNull);
					}

					const char* value = Json::get_string(json_node, "value");
					if (value)
					{
						node->set.value = strings_get(value);
						vi_assert(node->set.value != AssetNull);
					}
				}

				if (node->type == Node::Type::Branch)
				{
					cJSON* json_branches = cJSON_GetObjectItem(json_node, "branches");
					if (json_branches)
					{
						for (s32 j = 0; j < MAX_BRANCHES; j++)
						{
							node->branch.branches[j].target = IDNull;
							node->branch.branches[j].value = AssetNull;
						}

						cJSON* json_branch = json_branches->child;
						s32 j = 0;
						while (json_branch)
						{
							vi_assert(j < MAX_BRANCHES);
							node->branch.branches[j].value = strings_get(json_branch->string);
							vi_assert(node->branch.branches[j].value != AssetNull);
							if (json_branch->valuestring)
								node->branch.branches[j].target = id_lookup[json_branch->valuestring];
							else
								node->branch.branches[j].target = IDNull;
							json_branch = json_branch->next;
							j++;
						}
					}
				}

				// check for disconnected nodes
				if (
					node->next == IDNull
					&& node->type != Node::Type::Node // okay for us to end on a node
					&& (node->type != Node::Type::Text || node->text.choices[0] == IDNull) // if it's a text and it doesn't have a next, it must have a choice
					&& (node->type != Node::Type::Branch || node->branch.branches[0].target == IDNull) // if it's a branch, it must have at least one branch target
					)
				{
					vi_debug("Dangling dialogue node in file: %s - %s", AssetLookup::DialogueTree::values[tree_index], Json::get_string(json_node, "name"));
					vi_assert(false);
				}

				json_node = json_node->next;

				current_node_id++;
			}

		}

		for (s32 i = 0; i < trees.length; i++)
			Loader::dialogue_tree_free(trees[i]);
	}

	// UV origin: top left
	const Vec2 face_texture_size = Vec2(91.0f, 57.0f);
	const Vec2 face_size = Vec2(17.0f, 27.0f);
	const Vec2 face_uv_size = face_size / face_texture_size;
	const Vec2 face_pixel = Vec2(1.0f, 1.0f) / face_texture_size;
	const Vec2 face_offset = face_uv_size + face_pixel;
	const Vec2 face_origin = face_pixel;
	const Vec2 faces[(s32)Face::count] =
	{
		face_origin + Vec2(face_offset.x * 0, 0),
		face_origin + Vec2(face_offset.x * 1, 0),
		face_origin + Vec2(face_offset.x * 2, 0),
		face_origin + Vec2(face_offset.x * 3, 0),
		face_origin + Vec2(face_offset.x * 4, 0),
		face_origin + Vec2(face_offset.x * 0, face_offset.y),
		face_origin + Vec2(face_offset.x * 1, face_offset.y),
		face_origin + Vec2(face_offset.x * 2, face_offset.y),
		face_origin + Vec2(face_offset.x * 3, face_offset.y),
		face_origin + Vec2(face_offset.x * 4, face_offset.y),
	};

	template<typename T> struct Schedule
	{
		struct Entry
		{
			r32 time;
			T data;
		};

		s32 index;
		Array<Entry> entries;

		Schedule()
			: index(-1), entries()
		{
		}

		const T current() const
		{
			if (index < 0)
				return T();
			return entries[index].data;
		}

		b8 active() const
		{
			return index >= 0 && index < entries.length;
		}

		void schedule(r32 f, const T& data)
		{
			entries.add({ f, data });
		}

		void clear()
		{
			index = -1;
			entries.length = 0;
		}

		b8 update(const Update& u, r32 t)
		{
			if (index < entries.length - 1)
			{
				const Entry& next_entry = entries[index + 1];
				if (t > next_entry.time)
				{
					index++;
					return true;
				}
			}
			return false;
		}
	};

	typedef void (*Callback)(const Update&);

	struct Choice
	{
		ID a;
		ID b;
		ID c;
		ID d;
	};

	struct Data
	{
		r32 time;
		StaticArray<Ref<PlayerTrigger>, MAX_PLAYERS> terminals;
		Schedule<const char*> texts;
		Schedule<Face> faces;
		Schedule<AkUniqueID> audio_events;
		Schedule<Callback> callbacks;
		Schedule<Choice> choices;
		Schedule<AssetID> node_executions;
		Mode mode;
		Mode default_mode;
		ID current_text_node;
		AssetID entry_point;
		AssetID active_data_fragment;
		r32 fragment_time;
		Matchmake matchmake_mode;
		r32 matchmake_timer;

		UIText text;
		r32 text_clip;

		UIMenu menu;

		LinkArg<AssetID> node_executed;
	};

	static Data* data;

	b8 has_focus()
	{
		return data && (data->mode == Mode::Center || data->active_data_fragment != AssetNull);
	}

	void terminal_active(b8 active)
	{
		for (s32 i = 0; i < data->terminals.length; i++)
		{
			PlayerTrigger* terminal = data->terminals[i].ref();
			terminal->radius = active ? TERMINAL_TRIGGER_RADIUS : 0.0f;
			terminal->get<PointLight>()->radius = active ? TERMINAL_LIGHT_RADIUS : 0.0f;
		}
	}

	void matchmake_search()
	{
		variable(strings::matchmaking, strings::yes);
		data->matchmake_mode = Matchmake::Searching;
		data->matchmake_timer = 20.0f + mersenne::randf_oo() * 100.0f;
	}

	void clear()
	{
		if (data)
		{
			data->current_text_node = IDNull;
			data->time = 0.0f;
			data->text.text(nullptr);
			data->mode = Mode::Hidden;
			data->texts.clear();
			data->faces.clear();
			data->audio_events.clear();
			data->callbacks.clear();
			data->choices.clear();
			data->node_executions.clear();
		}
		Audio::post_global_event(AK::EVENTS::STOP_DIALOGUE);
		Audio::dialogue_done = false;
	}

	void variable(AssetID variable, AssetID value)
	{
		Game::save.variables[variable] = value;
	}

	AssetID variable(AssetID variable)
	{
		auto i = Game::save.variables.find(variable);
		if (i == Game::save.variables.end())
			return AssetNull;
		else
			return i->second;
	}

	void execute(ID node_id, r32 time = 0.0f)
	{
		const Node& node = Node::list[node_id];
		if (node.name != AssetNull)
			data->node_executions.schedule(time, node.name);
		switch (node.type)
		{
			case Node::Type::Text:
			{
				const char* str = _(node.name);
				data->texts.schedule(time, str);
				data->faces.schedule(time, node.text.face);
				if (node.text.sound != AK_INVALID_UNIQUE_ID)
					data->audio_events.schedule(time, node.text.sound);
				// stop executing the dialogue tree at this point
				// and save our current location in the tree
				// once the dialogue has been spoken, we will resume executing the tree
				data->current_text_node = node_id;
				break;
			}
			case Node::Type::Branch:
			{
				AssetID value = variable(node.branch.variable);
				b8 found_branch = false;
				for (s32 i = 0; i < MAX_BRANCHES; i++)
				{
					if (node.branch.branches[i].value == AssetNull)
						break;
					if (node.branch.branches[i].value == value)
					{
						if (node.branch.branches[i].target != IDNull)
							execute(node.branch.branches[i].target, time);
						found_branch = true;
						break;
					}
				}
				if (!found_branch)
				{
					// take default option
					for (s32 i = 0; i < MAX_BRANCHES; i++)
					{
						if (node.branch.branches[i].value == strings::_default)
						{
							if (node.branch.branches[i].target != IDNull)
								execute(node.branch.branches[i].target, time);
							break;
						}
					}
				}
				break;
			}
			case Node::Type::Choice:
			{
				// schedule a choice if one has not already been scheduled
				if (data->choices.entries.length == 0 || data->choices.entries[data->choices.entries.length - 1].time != time)
					data->choices.schedule(time, { IDNull, IDNull, IDNull, IDNull });

				// add this choice to the entry
				Schedule<Choice>::Entry& choice_entry = data->choices.entries[data->choices.entries.length - 1];
				if (choice_entry.data.a == IDNull)
					choice_entry.data.a = node_id;
				else if (choice_entry.data.b == IDNull)
					choice_entry.data.b = node_id;
				else if (choice_entry.data.c == IDNull)
					choice_entry.data.c = node_id;
				else if (choice_entry.data.d == IDNull)
					choice_entry.data.d = node_id;

				break;
			}
			case Node::Type::Set:
			{
				variable(node.set.variable, node.set.value);
				if (node.next != IDNull)
					execute(node.next, time);
				break;
			}
			case Node::Type::Node:
			{
				if (node.next != IDNull)
					execute(node.next, time);
				break;
			}
			default:
			{
				vi_assert(false);
				break;
			}
		}
	}

	void clear_and_execute(ID id, r32 delay = 0.0f)
	{
		clear();
		data->mode = data->default_mode;
		execute(id, delay);
	}

	void go(AssetID name, r32 delay = 0.0f)
	{
		clear_and_execute(node_lookup[name], delay);
	}

	void normal_conversation()
	{
		switch (Game::save.round)
		{
			case 0:
			{
				go(data->entry_point, 0.5f);
				break;
			}
			case 1:
			{
				go(strings::second_round, 0.5f);
				break;
			}
			default:
			{
				vi_assert(false);
				break;
			}
		}
	}

	void update(const Update& u)
	{
		PlayerTrigger* terminal = nullptr;
		for (s32 i = 0; i < data->terminals.length; i++)
		{
			if (data->terminals[i].ref()->count() > 0)
			{
				terminal = data->terminals[i].ref();
				break;
			}
		}

		if (terminal && !Console::visible && u.last_input->get(Controls::Interact, 0) && !u.input->get(Controls::Interact, 0))
		{
			terminal_active(false);
			if (Game::save.last_round_loss)
				go(strings::consolation, 0.5f);
			else
				normal_conversation();
		}

		if (Audio::dialogue_done)
		{
			// we've completed displaying a text message
			// continue executing the dialogue tree
			if (data->current_text_node != IDNull) // might be null if the dialogue got cut off or something
			{
				Node& node = Node::list[data->current_text_node];
				data->current_text_node = IDNull;
				if (node.next == IDNull)
				{
					for (s32 i = 0; i < MAX_CHOICES; i++)
					{
						ID choice = node.text.choices[i];
						if (choice == IDNull)
							break;
						else
							execute(choice, data->time);
					}
				}
				else
					execute(node.next, data->time);
			}

			Audio::dialogue_done = false;
		}

		if (data->node_executions.update(u, data->time))
			data->node_executed.fire(data->node_executions.current());
		data->faces.update(u, data->time);

		if (data->texts.update(u, data->time))
		{
			data->text.clip = data->text_clip = 1;
			data->text.wrap_width = MENU_ITEM_WIDTH - MENU_ITEM_PADDING * 2.0f;
			data->text.text_raw(data->texts.current());
		}

		if (data->callbacks.update(u, data->time))
			data->callbacks.current()(u);

		if (data->audio_events.update(u, data->time))
		{
			b8 success = Audio::post_dialogue_event(data->audio_events.current());
#if DEBUG
			// HACK to keep things working even with missing audio
			if (!success)
				Audio::post_dialogue_event(AK::EVENTS::PLAY_4E8907711D4DB021E656CB8CC752C7A4AF11E1F8);
#endif
		}

		if (data->text.clipped())
		{
			// We haven't shown the whole string yet
			r32 delta = u.time.delta * 80.0f;
			data->text_clip += delta;
			data->text.clip = data->text_clip;

			if ((s32)data->text_clip % 3 == 0
				&& (s32)(data->text_clip - delta) < (s32)data->text_clip
				&& data->text.rendered_string[(s32)data->text_clip] != ' '
				&& data->text.rendered_string[(s32)data->text_clip] != '\t'
				&& data->text.rendered_string[(s32)data->text_clip] != '\n')
			{
				Audio::post_global_event(AK::EVENTS::PLAY_CONSOLE_KEY);
			}
		}

		if (data->choices.update(u, data->time))
			data->menu.animate();

		// data fragment
		DataFragment* fragment = nullptr;
		if (PlayerCommon::list.count() > 0)
			fragment = DataFragment::in_range(PlayerCommon::list.iterator().item()->get<Transform>()->absolute_pos());

		// show choices
		data->menu.clear();
		if (data->choices.active() && data->active_data_fragment == AssetNull)
		{
			const Choice& choice = data->choices.current();
			if (choice.a != IDNull)
			{
				data->menu.start(u, 0, has_focus());

				Vec2 p;
				if (data->mode == Mode::Left)
					p = Vec2(u.input->width * 0.9f - MENU_ITEM_WIDTH, u.input->height * 0.3f);
				else
					p = Vec2(u.input->width * 0.5f + MENU_ITEM_WIDTH * -0.5f, u.input->height * 0.3f);

				{
					Node& node = Node::list[choice.a];
					if (data->menu.item(u, &p, _(node.name)))
						clear_and_execute(node.next, 0.25f);
				}
				if (choice.b != IDNull)
				{
					Node& node = Node::list[choice.b];
					if (data->menu.item(u, &p, _(node.name)))
						clear_and_execute(node.next, 0.25f);
				}
				if (choice.c != IDNull)
				{
					Node& node = Node::list[choice.c];
					if (data->menu.item(u, &p, _(node.name)))
						clear_and_execute(node.next, 0.25f);
				}
				if (choice.d != IDNull)
				{
					Node& node = Node::list[choice.d];
					if (data->menu.item(u, &p, _(node.name)))
						clear_and_execute(node.next, 0.25f);
				}

				data->menu.end();
			}
		}

		// get focus if we need it
		if (data->choices.active()
			&& !has_focus() // penelope is waiting on us, but she doesn't have focus yet
			&& !fragment) // fragments take precedence over dialogue
		{
			if (!Console::visible && u.last_input->get(Controls::Interact, 0) && !u.input->get(Controls::Interact, 0))
				data->mode = Mode::Center; // we have focus now!
		}

		// data fragments
		{
			if (data->active_data_fragment == AssetNull)
			{
				if (fragment)
				{
					if (!Console::visible && u.last_input->get(Controls::Interact, 0) && !u.input->get(Controls::Interact, 0))
					{
						fragment->collect();
						data->active_data_fragment = fragment->text();
						data->fragment_time = Game::real_time.total;
					}
				}
			}
			else
			{
				// we're reading a data fragment
				if (!Console::visible && u.last_input->get(Controls::Cancel, 0) && !u.input->get(Controls::Cancel, 0))
					data->active_data_fragment = AssetNull;
			}
		}

		switch (data->matchmake_mode)
		{
			case Matchmake::None:
			{
				break;
			}
			case Matchmake::Searching:
			{
				data->matchmake_timer -= Game::real_time.delta;
				if (data->matchmake_timer < 0.0f)
				{
					data->matchmake_mode = Matchmake::Found;
					data->matchmake_timer = 15.0f;
					data->active_data_fragment = AssetNull; // if we're looking at a data fragment, close it
					go(strings::match_found, 0.25f);
				}
				break;
			}
			case Matchmake::Found:
			{
				data->matchmake_timer -= Game::real_time.delta;
				if (data->matchmake_timer < 0.0f)
					Menu::transition(Game::state.level, Game::Mode::Pvp);
				break;
			}
			default:
			{
				vi_assert(false);
				break;
			}
		}

		data->time += u.time.delta;
	}

	void draw(const RenderParams& params)
	{
		const Rect2& vp = params.camera->viewport;

		DataFragment* fragment = nullptr;
		if (PlayerCommon::list.count() > 0)
			fragment = DataFragment::in_range(PlayerCommon::list.iterator().item()->get<Transform>()->absolute_pos());

		PlayerTrigger* terminal = nullptr;
		for (s32 i = 0; i < data->terminals.length; i++)
		{
			if (data->terminals[i].ref()->count() > 0)
			{
				terminal = data->terminals[i].ref();
				break;
			}
		}

		// fragment UI
		if (data->active_data_fragment == AssetNull)
		{
			if (fragment)
			{
				// prompt to read it
				UIText text;
				text.anchor_x = UIText::Anchor::Center;
				text.anchor_y = UIText::Anchor::Max;
				text.size = 16.0f;
				text.color = UI::accent_color;
				text.text("[{{Interact}}]");

				Vec2 p = vp.size * Vec2(0.5f, 0.8f);
				UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::background_color);
				text.draw(params, p);
			}
		}
		else
		{
			// reading the data fragment
			UIText text;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Max;
			text.size = 20.0f;
			text.font = Asset::Font::pt_sans;
			text.color = UI::default_color;
			text.wrap_width = MENU_ITEM_WIDTH * 1.5f;
			text.text(_(strings::data_fragment), DataFragment::count_collected(), DataFragment::list.count(), _(fragment->text()));
			text.clip = 1 + (s32)((Game::real_time.total - data->fragment_time) * 300.0f);
			Vec2 p = vp.size * Vec2(0.5f, 0.9f);
			UI::box(params, text.rect(p).outset(16.0f * UI::scale), UI::background_color);
			text.draw(params, p);

			text.color = UI::accent_color;
			text.font = Asset::Font::lowpoly;
			text.size = 16.0f;
			text.clip = 0;
			text.wrap_width = 0;
			text.text("[{{Cancel}}]");
			p = vp.size * Vec2(0.5f, 0.1f);
			UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::background_color);
			text.draw(params, p);
		}

		// prompt the player to interact if near a terminal or if penelope is waiting on the player to answer a question
		if (!fragment) // if there's a fragment, that takes precedence
		{
			b8 show_interact_prompt = false;
			Vec2 p;
			if (terminal)
			{
				show_interact_prompt = true;
				p = vp.size * Vec2(0.5f, 0.4f);
			}
			else if (data->choices.active() && !has_focus())
			{
				show_interact_prompt = true;
				p = data->menu.items[0].pos + Vec2(-32.0f * UI::scale - MENU_ITEM_PADDING_LEFT, data->menu.items.length * MENU_ITEM_HEIGHT * -0.5f);
			}

			if (show_interact_prompt)
			{
				// we are near a terminal, or penelope is waiting on the player to answer a question
				// prompt the player to hit the "Interact" button
				UIText text;
				text.color = UI::accent_color;
				text.anchor_x = UIText::Anchor::Center;
				text.anchor_y = UIText::Anchor::Min;
				text.size = 16.0f;
				text.text("[{{Interact}}]");

				UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::background_color);
				text.draw(params, p);
			}
		}

		// matchmake UI
		if (data->matchmake_mode != Matchmake::None)
		{
			UIText text;
			text.anchor_x = UIText::Anchor::Center;
			text.anchor_y = UIText::Anchor::Center;
			text.color = UI::accent_color;
			text.text(_(data->matchmake_mode == Matchmake::Searching ? strings::match_searching : strings::match_starting), ((s32)data->matchmake_timer) + 1);
			Vec2 pos = vp.pos + vp.size * Vec2(0.1f, 0.25f) + Vec2(12.0f * UI::scale, -100 * UI::scale);

			UI::box(params, text.rect(pos).pad({ Vec2((8.0f + 24.0f) * UI::scale, 8.0f * UI::scale), Vec2(8.0f * UI::scale) }), UI::background_color);

			Vec2 triangle_pos
			(
				pos.x + text.bounds().x * -0.5f - 16.0f * UI::scale,
				pos.y
			);

			if (data->matchmake_mode == Matchmake::Searching)
				UI::triangle_border(params, { triangle_pos, Vec2(text.size * 0.5f * UI::scale) }, 3, UI::accent_color, Game::real_time.total * -8.0f);
			else
				UI::triangle(params, { triangle_pos, Vec2(text.size * UI::scale) }, UI::accent_color);

			text.draw(params, pos);
		}

		// penelope face and UI

		if (data->mode != Mode::Hidden && data->active_data_fragment == AssetNull) // hide penelope while reading a data fragment
		{
			Face face = data->faces.current();

			r32 scale = UI::scale;
			Vec2 pos;
			switch (data->mode)
			{
				case Mode::Center:
				{
					pos = vp.pos + vp.size * Vec2(0.5f, 0.6f);
					scale *= 4.0f;
					break;
				}
				case Mode::Left:
				{
					pos = vp.pos + vp.size * Vec2(0.1f, 0.25f);
					scale *= 3.0f;
					break;
				}
				case Mode::Hidden:
				{
					break;
				}
				default:
				{
					vi_assert(false);
					break;
				}
			}

			if (face == Face::Default)
			{
				// blink
				const r32 blink_delay = 4.0f;
				const r32 blink_time = 0.1f;
				if (fmod(data->time, blink_delay) < blink_time)
					face = Face::EyesClosed;
			}

			// frame
			{
				// visualize dialogue volume
				r32 volume_scale;
				if (Settings::sfx > 0)
					volume_scale = 1.0f + (Audio::dialogue_volume / ((r32)Settings::sfx / 100.0f)) * 0.5f;
				else
					volume_scale = 1.0f;
				Vec2 frame_size(32.0f * scale * volume_scale);
				UI::centered_box(params, { pos, frame_size }, UI::background_color, PI * 0.25f);

				const Vec4* color;
				switch (face)
				{
					case Face::Default:
					case Face::Upbeat:
					case Face::Concerned:
					{
						color = &UI::default_color;
						break;
					}
					case Face::Sad:
					{
						color = &Team::ui_color_friend;
						break;
					}
					case Face::EyesClosed:
					case Face::Unamused:
					case Face::Wat:
					{
						color = &UI::disabled_color;
						break;
					}
					case Face::Urgent:
					case Face::Smile:
					{
						color = &UI::accent_color;
						break;
					}
					case Face::Angry:
					{
						color = &UI::alert_color;
						break;
					}
					default:
					{
						vi_assert(false);
						break;
					}
				}

				UI::centered_border(params, { pos, frame_size }, 2.0f, *color, PI * 0.25f);
			}

			// face
			{
				Vec2 face_uv = faces[(s32)face];
				UI::sprite(params, Asset::Texture::penelope, { pos, face_size * scale }, UI::default_color, { face_uv, face_uv_size });
			}
		}

		// text
		if (data->text.has_text() && data->active_data_fragment == AssetNull)
		{
			Vec2 pos;
			switch (data->mode)
			{
				case Mode::Center:
				{
					data->text.anchor_x = UIText::Anchor::Center;
					data->text.anchor_y = UIText::Anchor::Max;
					data->text.color = UI::default_color;
					pos = Vec2(vp.size.x * 0.5f, vp.size.y * 0.6f) + Vec2(0, -100 * UI::scale);
					break;
				}
				case Mode::Hidden:
				{
					data->text.anchor_x = UIText::Anchor::Center;
					data->text.anchor_y = UIText::Anchor::Max;
					data->text.color = UI::accent_color;
					pos = Vec2(vp.size.x * 0.5f, vp.size.y * 0.9f);
					break;
				}
				case Mode::Left:
				{
					data->text.anchor_x = UIText::Anchor::Min;
					data->text.anchor_y = UIText::Anchor::Center;
					data->text.color = UI::default_color;
					pos = Vec2(vp.size.x * 0.18f, vp.size.y * 0.25f);
					break;
				}
				default:
				{
					vi_assert(false);
					break;
				}
			}

			UI::box(params, data->text.rect(pos).outset(MENU_ITEM_PADDING), UI::background_color);
			data->text.draw(params, pos);
		}

		// menu
		data->menu.draw_alpha(params);
	}

	void cleanup()
	{
		delete data;
		data = nullptr;
	}

	void node_executed(AssetID node)
	{
		if (node == strings::terminal_reset)
			terminal_active(true);
		else if (node == strings::penelope_hide)
			clear();
		else if (node == strings::consolation_done) // we're done consoling; enter into normal conversation mode
			normal_conversation();
		else if (node == strings::match_go)
		{
			if (Game::save.round == 0)
				Menu::transition(Game::state.level, Game::Mode::Pvp); // reload current level in PvP mode
			else
			{
				// must play another round before advancing to the next level
				// play a random map that has already been unlocked so far (except the starting/tutorial maps)
				const s32 skip_levels = 2;
				Menu::transition(Game::levels[skip_levels + (s32)(mersenne::randf_co() * (Game::save.level_index - skip_levels))], Game::Mode::Pvp);
			}
		}
		else if (node == strings::matchmaking_start)
			matchmake_search();
	}

	void init(AssetID entry_point, Mode default_mode)
	{
		vi_assert(!data);
		data = new Data();
		data->entry_point = entry_point;
		data->default_mode = default_mode;
		data->active_data_fragment = AssetNull;
		Audio::dialogue_done = false;
		Game::updates.add(update);
		Game::cleanups.add(cleanup);
		Game::draws.add(draw);

		data->node_executed.link(&node_executed);

		variable(strings::matchmaking, AssetNull);

		Loader::texture(Asset::Texture::penelope, RenderTextureWrap::Clamp, RenderTextureFilter::Nearest);
	}

	void add_terminal(Entity* term)
	{
		data->terminals.add(term->get<PlayerTrigger>());
	}
}

namespace scene
{
	struct Data
	{
		UIText text;
		r32 clip;
		Camera* camera;
	};
	
	static Data* data;

	void cleanup()
	{
		data->camera->remove();
	}

	void update(const Update& u)
	{
		data->camera->rot = Quat::euler(0.0f, Game::real_time.total * 0.01f, 0.0f);
	}

	void init(const Update& u, const EntityFinder& entities)
	{
		data = new Data();

		data->camera = Camera::add();

		data->camera->viewport =
		{
			Vec2(0, 0),
			Vec2(u.input->width, u.input->height),
		};
		r32 aspect = data->camera->viewport.size.y == 0 ? 1 : (r32)data->camera->viewport.size.x / (r32)data->camera->viewport.size.y;
		data->camera->perspective((80.0f * PI * 0.5f / 180.0f), aspect, 0.1f, Game::level.skybox.far_plane);

		Game::cleanups.add(cleanup);
		Game::updates.add(update);
	}
}

namespace intro
{
	void node_executed(AssetID node)
	{
		if (node == strings::intro_done)
		{
			b8 skip_tutorial = Penelope::variable(strings::skip_tutorial) == strings::yes;
			Game::save.level_index++;
			Menu::transition(Game::levels[Game::save.level_index], Game::Mode::Pvp);
		}
	}

	void init(const Update& u, const EntityFinder& entities)
	{
		Penelope::init(AssetNull, Penelope::Mode::Center);
		Penelope::data->node_executed.link(&node_executed);
		Penelope::go(strings::intro, 1.0f);
	}
}

namespace connect
{
	struct LevelNode
	{
		s32 index;
		AssetID id;
		Ref<Transform> pos;
	};

	struct Data
	{
		UIText text;
		r32 clip;
		Camera* camera;
		Vec3 start;
		Vec3 target;
		Vec3 camera_offset;
		Array<LevelNode> levels;
	};
	
	static Data* data;

	void cleanup()
	{
		data->camera->remove();
	}

	void update(const Update& u)
	{
		data->camera->active = u.time.total > 0.5f && Menu::connect_timer > 0.5f;
		data->camera->pos = data->camera_offset + Ease::expo_out(vi_min(1.0f, (u.time.total - 0.5f) / 2.0f), data->start, data->target);
	}

	void draw(const RenderParams& params)
	{
		for (s32 i = 0; i < data->levels.length; i++)
		{
			const LevelNode& node = data->levels[i];
			Transform* pos = node.pos.ref();
			UI::indicator(params, pos->absolute_pos(), node.index <= Game::save.level_index ? UI::accent_color : UI::alert_color, false);

			if (node.id == Menu::next_level)
			{
				Vec2 p;
				if (UI::project(params, pos->absolute_pos(), &p))
				{
					p.y += 32.0f * UI::scale;
					UIText text;
					text.color = UI::accent_color;
					text.anchor_x = UIText::Anchor::Center;
					text.anchor_y = UIText::Anchor::Min;
					text.text_raw(AssetLookup::Level::names[node.id]);
					UI::box(params, text.rect(p).outset(8.0f * UI::scale), UI::background_color);
					text.draw(params, p);
				}
			}
		}

		UIText text;
		text.anchor_x = text.anchor_y = UIText::Anchor::Center;
		text.color = UI::accent_color;
		text.text(_(Menu::next_mode == Game::Mode::Pvp ? strings::connecting : strings::loading_offline));
		Vec2 pos = params.camera->viewport.size * Vec2(0.5f, 0.2f);

		UI::box(params, text.rect(pos).pad({ Vec2(64, 24) * UI::scale, Vec2(18, 24) * UI::scale }), UI::background_color);

		text.draw(params, pos);

		Vec2 triangle_pos = Vec2
		(
			pos.x - text.bounds().x * 0.5f - 32.0f * UI::scale,
			pos.y
		);
		UI::triangle_border(params, { triangle_pos, Vec2(20 * UI::scale) }, 6, UI::accent_color, Game::real_time.total * -8.0f);
	}

	void init(const Update& u, const EntityFinder& entities)
	{
		data = new Data();

		data->camera = Camera::add();

		{
			Entity* map_view_entity = entities.find("map_view");
			if (map_view_entity)
			{
				Transform* map_view = map_view_entity->get<Transform>();
				data->camera->rot = Quat::look(map_view->absolute_rot() * Vec3(0, -1, 0));
				data->camera_offset = map_view->absolute_pos();
			}
		}

		for (s32 i = 0; i < Asset::Level::count; i++)
		{
			AssetID level_id = Game::levels[i];
			if (level_id == AssetNull)
				break;

			Entity* entity = entities.find(AssetLookup::Level::names[level_id]);
			if (entity)
			{
				data->levels.add({ i, level_id, entity->get<Transform>() });
				if (level_id == Menu::next_level)
					data->target = entity->get<Transform>()->absolute_pos();
				if (level_id == Menu::transition_previous_level)
					data->start = entity->get<Transform>()->absolute_pos();
			}
		}

		data->camera->viewport =
		{
			Vec2(0, 0),
			Vec2(u.input->width, u.input->height),
		};
		r32 aspect = data->camera->viewport.size.y == 0 ? 1 : (r32)data->camera->viewport.size.x / (r32)data->camera->viewport.size.y;
		data->camera->perspective((80.0f * PI * 0.5f / 180.0f), aspect, 0.1f, Game::level.skybox.far_plane);

		Game::cleanups.add(cleanup);
		Game::updates.add(update);
		Game::draws.add(draw);
	}
}

namespace tutorial
{
	enum class TutorialState
	{
		ParkourJump, ParkourClimb, ParkourClimbDone, ParkourWallRun,
		PvpKillMinion, PvpGetHealth, PvpControlPoint, PvpUpgrade, PvpKillPlayer,
		Done,
	};

	struct Data
	{
		TutorialState state;
		Ref<Mover> door_mover;
		Ref<Transform> health_location;
		Ref<ControlPoint> control_point;
	};

	Data* data;

	void jump_success(Entity*)
	{
		if (data->state == TutorialState::ParkourJump)
		{
			data->state = TutorialState::ParkourClimb;
			Penelope::data->texts.clear();
			Penelope::data->texts.schedule(0.0f, _(strings::tut_parkour_climb));
		}
	}

	void climb_success(Entity*)
	{
		if (data->state == TutorialState::ParkourClimb)
		{
			data->state = TutorialState::ParkourClimbDone;
			Penelope::clear();
			data->door_mover.ref()->go();
		}
	}

	void wallrun_tutorial(Entity*)
	{
		if (data->state == TutorialState::ParkourClimbDone)
		{
			data->state = TutorialState::ParkourWallRun;
			Penelope::clear();
			Penelope::data->texts.schedule(0.0f, _(strings::tut_parkour_wallrun));
		}
	}

	void wallrun_success(Entity*)
	{
		if (data->state == TutorialState::ParkourWallRun)
		{
			data->state = TutorialState::Done;
			Penelope::clear();
		}
	}

	void health_got(const TargetEvent& e)
	{
		PlayerManager* manager = LocalPlayer::list.iterator().item()->manager.ref();
		manager->credits = UpgradeInfo::list[(s32)Upgrade::Sensor].cost + AbilityInfo::list[(s32)Ability::Sensor].spawn_cost * 2;
		manager->upgrade_start(Upgrade::Sensor);
		manager->upgrade_complete();

		data->state = TutorialState::PvpControlPoint;
		Penelope::data->texts.clear();
		Penelope::data->texts.schedule(0.0f, _(strings::tut_pvp_control_points));
		Game::level.feature_level = Game::FeatureLevel::ControlPoints;
	}

	void spawned()
	{
		LocalPlayerControl::list.iterator().item()->get<Health>()->hp = 2;
	}

	void minion_killed(Entity*)
	{
		// spawn health
		Vec3 pos = data->health_location.ref()->absolute_pos();
		Entity* health = World::create<HealthPickupEntity>(pos);
		Rope::spawn(pos + Vec3(0, 1, 0), Vec3(0, 1, 0), 20.0f);
		health->get<Target>()->target_hit.link(&health_got);

		data->state = TutorialState::PvpGetHealth;
		Penelope::data->texts.clear();
		Penelope::data->texts.schedule(0.0f, _(strings::tut_pvp_health));
	}

	void update(const Update& u)
	{
		if (data->state == TutorialState::PvpControlPoint)
		{
			if (data->control_point.ref()->team != AI::Team::None)
			{
				PlayerManager* manager = LocalPlayer::list.iterator().item()->manager.ref();
				manager->credits += UpgradeInfo::list[(s32)Upgrade::Minion].cost;

				data->state = TutorialState::PvpUpgrade;
				Penelope::data->texts.clear();
				Penelope::data->texts.schedule(0.0f, _(strings::tut_pvp_upgrade));
			}
		}
		else if (data->state == TutorialState::PvpUpgrade)
		{
			PlayerManager* manager = LocalPlayer::list.iterator().item()->manager.ref();
			for (s32 i = (s32)Upgrade::Sensor + 1; i < (s32)Upgrade::count; i++)
			{
				if (manager->has_upgrade((Upgrade)i))
				{
					data->state = TutorialState::PvpKillPlayer;
					data->door_mover.ref()->go();
					Penelope::data->texts.clear();
					Penelope::data->texts.schedule(0.0f, _(strings::tut_pvp_kill_player));
					break;
				}
			}
		}
	}

	void draw(const RenderParams& params)
	{
		if (data->state == TutorialState::PvpControlPoint)
			UI::indicator(params, data->control_point.ref()->get<Transform>()->absolute_pos(), UI::accent_color, true);
	}

	void cleanup()
	{
		delete data;
		data = nullptr;
	}

	void node_executed(AssetID node)
	{
		if (node == strings::tutorial_intro_done)
		{
			Penelope::data->default_mode = Penelope::Mode::Left;
			Penelope::clear();
			Penelope::data->texts.schedule(0.0f, _(strings::tut_parkour_movement));
		}
		else if (node == strings::tutorial_done)
		{
			Game::save.level_index++;
			Game::save.round = 0;
			Menu::transition(Game::levels[Game::save.level_index], Game::Mode::Parkour);
		}
	}

	void tutorial_intro(const Update&)
	{
		Penelope::data->default_mode = Penelope::Mode::Center;
		Penelope::go(strings::tutorial_intro, 1.0f);
	}

	void init(const Update& u, const EntityFinder& entities)
	{
		Game::level.feature_level = Game::FeatureLevel::HealthPickups;

		data = new Data();
		Game::draws.add(&draw);
		Game::updates.add(&update);
		Game::cleanups.add(&cleanup);

		data->door_mover = entities.find("door_mover")->get<Mover>();

		if (Game::state.mode == Game::Mode::Pvp)
		{
			data->health_location = entities.find("health")->get<Transform>();
			data->control_point = entities.find("control_point")->get<ControlPoint>();

			entities.find("minion")->get<Health>()->killed.link(&minion_killed);

			PlayerManager* manager = PlayerManager::list.add();
			new (manager) PlayerManager(&Team::list[(s32)AI::Team::B]);

			utf8cpy(manager->username, _(strings::dummy));

			AIPlayer* player = AIPlayer::list.add();
			new (player) AIPlayer(manager);

			AIPlayer::Config* config = &player->config;
			config->high_level = AIPlayer::HighLevelLoop::Noop;
			config->low_level = AIPlayer::LowLevelLoop::Noop;

			LocalPlayer::list.iterator().item()->manager.ref()->spawn.link(&spawned);

			Penelope::init(); // have to init manually since penelope normally isn't loaded in PvP mode
			Penelope::data->texts.schedule(3.0f, _(strings::tut_pvp_minion));
		}
		else
		{
			Penelope::data->node_executed.link(&node_executed);
			entities.find("jump_success")->get<PlayerTrigger>()->entered.link(&jump_success);
			entities.find("climb_success")->get<PlayerTrigger>()->entered.link(&climb_success);
			entities.find("wallrun_tutorial")->get<PlayerTrigger>()->entered.link(&wallrun_tutorial);
			entities.find("wallrun_success")->get<PlayerTrigger>()->entered.link(&wallrun_success);

			Penelope::data->callbacks.schedule(4.0f, tutorial_intro);
		}
	}
}

Script Script::all[] =
{
	{ "scene", scene::init },
	{ "connect", connect::init },
	{ "intro", intro::init },
	{ "tutorial", tutorial::init },
	{ 0, 0, },
};

}

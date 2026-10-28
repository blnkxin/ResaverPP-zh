#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <unordered_map>

namespace tips
{
	bool Ready()
	{
		if (!g_enabled) {
			return false;
		}
		ImGui::GetStyle().HoverDelayNormal = g_delay;
		return ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_AllowWhenDisabled);
	}

	namespace
	{
		void Show(const char* a_text)
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
			ImGui::TextUnformatted(a_text);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	void Tip(const char* a_text)
	{
		if (a_text && a_text[0] && Ready()) {
			Show(a_text);
		}
	}

	void Tipf(const char* a_fmt, ...)
	{
		if (!a_fmt || !Ready()) {
			return;
		}
		char buf[4096];
		va_list ap;
		va_start(ap, a_fmt);
		std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
		va_end(ap);
		Show(buf);
	}

	namespace
	{
		std::string Key(std::string_view a_label)
		{
			std::string s(a_label);
			// "EXTRADATA (5)" -> "EXTRADATA", "Change Forms (69380)" -> "Change Forms", "LocRef" stays
			if (const auto p = s.find(" ("); p != std::string::npos && !s.empty() && s.back() == ')') {
				s.erase(p);
			}
			// "25: REFR_PROMOTED" -> "REFR_PROMOTED"
			if (const auto c = s.find(": "); c != std::string::npos && c <= 3 &&
				std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(c), [](unsigned char ch) { return std::isdigit(ch); })) {
				s.erase(0, c + 2);
			}
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
			return s;
		}

		const std::unordered_map<std::string, const char*>& Glossary()
		{
			static const std::unordered_map<std::string, const char*> g = [] {
				std::unordered_map<std::string, const char*> m;
				auto add = [&](const char* k, const char* v) { m[Key(k)] = v; };
				// ---- sidebar ----
				add("Overview", "存档的概况：你是谁、在哪里、它有多大，以及有什么异常。");
				add("Mods", "存档提到的每个模组（插件），以及它留在存档里的脚本、脚本实例和变更表单各有几个。已经卸载的模组也会显示在这里。");
				add("Quests", "这个存档里的任务：进行中、已完成和已失败，连同它们的阶段和日志目标。");
				add("Plugins", "存档的加载顺序：保存时启用的每个插件（.esm/.esp/.esl）。点一个就能看到它往存档里放了什么。");
				add("Change Forms",
					"游戏相对插件内容改掉的一切：移动或杀死的角色、打开的门、捡起的物品、改变的任务等等。每一项改动一条变更表单；ACHR 是角色，REFR 是物体，NPC_ 是角色基础。");
				add("Strings", "脚本引擎的文本表：每个脚本、变量和函数的名字，以及脚本保存的每个文本值。");
				add("Scripts", "存档里有数据的脚本类型（每个 .pex 文件一种）。");
				add("Script Instances", "附加在某个东西上的脚本（角色、物体、任务……），每个都有自己的一套变量。");
				add("References", "不附加在表单上的脚本对象：某些脚本为了存数据而创建的结构。");
				add("Arrays", "脚本保存在存档里的脚本数组（一组值）。");
				add("Active Scripts", "你存档时正在运行的脚本（线程）。同一个脚本开了很多线程，或者线程卡在等待上，都是脚本卡顿的常见原因。");
				add("Function Messages", "你存档时已经排队、但还没开始的脚本调用。");
				add("Suspended Stacks", "你存档时被暂停、等着继续的脚本调用。");
				add("Queued Unbinds", "等着从对象上解绑的脚本。");
				// ---- change form types ----
				add("ACHR", "角色引用：放置好的 NPC 或生物，或者你自己。保存位置、物品栏、AI、属性等。");
				add("REFR", "物体引用：放置好的物体（物品、门、容器、家具……），且状态发生过变化。");
				add("NPC_", "角色基础：NPC 记录本身（名字、脸、种族、技能、派系、装备），在游戏改动过它时出现。");
				add("QUST", "任务：它的阶段、目标、标记和别名。");
				add("CELL", "单元格（室内，或世界中的一格）：它的哪些部分发生了变化。");
				add("FLST", "FormList：某些脚本会往里增删内容的一份表单列表。");
				add("FACT", "派系：它的标记、与其他派系的关系，以及犯罪计数。");
				// ---- change flags ----
				add("CHANGE_FORM_FLAGS", "记录自身的标记变了（例如禁用、删除、持久化）。");
				add("REFR_MOVE", "它被移动过：保存的位置、旋转和所属单元格替换了插件里的。");
				add("REFR_HAVOK_MOVE", "它的物理（Havok）状态被保存了：它怎么躺着、怎么翻滚。");
				add("REFR_CELL_CHANGED", "它移到了插件放置它之外的另一格单元格。");
				add("REFR_SCALE", "它的大小被改过。");
				add("REFR_INVENTORY", "它携带的物品变了（增加了或减少了）。");
				add("REFR_EXTRA_OWNERSHIP", "它的所有者变了。");
				add("REFR_BASEOBJECT", "它是什么的实例（它的基础对象）变了。");
				add("REFR_PROMOTED", "它被提升为持久化，通常是因为有任务或脚本指向它。");
				add("REFR_ANIMATION", "它的动画状态被保存了（姿势、待机动作、正在做什么）。");
				add("REFR_LEVELED_INVENTORY", "它的等级物品已经抽取并保存。");
				add("REFR_EXTRA_ENCOUNTER_ZONE", "它的遭遇区域（等级范围和重置规则）变了。");
				add("REFR_EXTRA_GAME_ONLY", "它带有游戏只保存在存档里的额外数据。");
				add("REFR_EXTRA_CREATED_ONLY", "游戏过程中创建的对象所带的额外数据。");
				add("REFR_EXTRA_ACTIVATING_CHILDREN", "使用它时会激活什么，变了。");
				add("ACTOR_EXTRA_PACKAGE_DATA", "它正在运行的 AI 包的数据。");
				add("LIFESTATE", "活着、死亡、昏迷、流血不止……");
				add("ACTOR_EXTRA_MERCHANT_CONTAINER", "它的商人箱子。");
				add("ACTOR_EXTRA_DISMEMBERED_LIMBS", "它失去的肢体。");
				add("ACTOR_LEVELED_ACTOR", "它的等级列表抽出了哪个角色。");
				add("ACTOR_DISPOSITION_MODIFIERS", "它对别人的态度。");
				add("ACTOR_TEMP_MODIFIERS", "对它角色值的临时改动。");
				add("ACTOR_DAMAGE_MODIFIERS", "对它角色值的伤害（损失的生命、魔法、耐力）。");
				add("ACTOR_OVERRIDE_MODIFIERS", "对它角色值的覆盖。");
				add("ACTOR_PERMANENT_MODIFIERS", "对它角色值的永久改动。");
				add("ACTOR_BASE_DATA", "基础属性：等级、生命/魔法/耐力的偏移、速度。");
				add("ACTOR_BASE_AIDATA", "AI 设置：攻击性、自信、道德……");
				add("ACTOR_BASE_SPELLLIST", "它会用的法术和龙吼。");
				add("ACTOR_BASE_FULLNAME", "它的名字被改过。");
				add("ACTOR_BASE_FACTIONS", "它所属的派系。");
				add("NPC_SKILLS", "它的技能。");
				add("NPC_CLASS", "它的职业。");
				add("NPC_FACE", "它的脸（头部部件、变形、色斑）。移除后就恢复成插件里的脸。");
				add("DEFAULT_OUTFIT", "它平时穿什么。");
				add("SLEEP_OUTFIT", "它睡觉时穿什么。");
				add("NPC_GENDER", "它的性别被改过。");
				add("NPC_RACE", "它的种族被改过。");
				// ---- decoded groups and values ----
				add("INITIAL", "它在哪里：单元格、位置和旋转，只在移动过时才保存。");
				add("CELL", "它所在的单元格（室内，或世界中的一格）。");
				add("POS", "位置，以游戏单位计（x、y、z）。");
				add("ROT", "旋转，以弧度计（x、y、z）。");
				add("HAVOK", "物理状态（怎么躺着、怎么移动）。重置 Havok 会把它清掉。");
				add("PROCESS LEVEL", "这个角色加载了多少 AI：0 高（离你近）、1 中高、2 中低、3 低、-1 无（未加载）。");
				add("BOOL BITS", "角色状态标记（例如死亡、战斗中、幽灵、重要角色……）。");
				add("EXTRADATA", "附加在它身上的额外数据：所有者、遭遇区域、别名、场景、AI 包等等。");
				add("INVENTORY", "它携带的物品，在插件给它的物品之外（或替代它们）。");
				add("ANIMATIONS", "它的动画图状态（当前处于什么姿势、在播什么动画）。");
				add("AI PROCESS", "它的 AI 状态：当前的 AI 包和流程、目标、已装备的物品、对话、对其他人的认知。");
				add("CURRENT PACKAGE", "它正在运行的 AI 包（跟随、原地活动、旅行、进食……）以及执行到哪一步。");
				add("PACKAGE", "一个 AI 包：告诉角色去做什么。");
				add("PACKAGE DATA", "正在运行的那个 AI 包自身的状态（它的目标和流程）。");
				add("PACKAGE TYPE", "AI 包的流程类型（18 = 由模板生成的包，最常见的那种）。");
				add("MIDDLE HIGH", "角色离你较近时保留的 AI 数据。");
				add("HIGH", "角色离你近且活跃时保留的 AI 数据。");
				add("ACTOR", "角色自身的数据：计时器、状态、正在施放的法术、角色值、添加的法术、移动和战斗。");
				add("ACTOR VALUES", "属性：生命、魔法、技能、抗性、速度……基础值是存档里的值，修正项会改动它。");
				add("BASE", "它的角色值已保存的基础值。");
				add("MODIFIERS", "在基础值之上的改动：永久、临时（激活效果）和伤害。");
				add("ADDED SPELLS", "游戏过程中获得的法术、能力和增益。");
				add("MOVEMENT CONTROLLER", "角色怎么移动：它的路径、寻路请求，以及控制它的各个部件。");
				add("COMBAT CONTROLLER", "它的战斗状态：目标、瞄准、战斗区域、可能使用的武器和法术、行为。");
				add("ARBITERS", "移动控制器里决定它怎么移动的部件。");
				add("AGENTS", "移动控制器里实际执行移动的部件（沿路径跟随、避开别人……）。");
				add("PLAYER", "只有玩家才有的数据：技能进度、相机、犯罪记录、专长点、种族等等。");
				add("CAMERA", "各种相机状态（第一人称、第三人称、骑马、骑龙……）。");
				add("SKILLS", "技能等级，以及距离下一级的进度。");
				add("CRIME GOLD", "你在各派系的赏金和犯罪总额。");
				add("STOLEN ITEM VALUES", "从各派系偷走的物品价值。");
				add("SKIN", "你的种族和皮肤色斑数据。");
				add("PERK POINTS", "没花掉的专长点。");
				add("XP", "距离下一个角色等级的经验。");
				add("XP FOR NEXT LEVEL", "升到下一个角色等级所需的经验。");
				add("FACTIONRANKS", "角色所属的派系，以及它在每个派系里的阶位。");
				add("ACBS", "基础属性：标记、生命/魔法/耐力偏移、等级、速度、态度。");
				add("AIDT", "AI 数据：攻击性、自信、活力、道德、情绪、协助。");
				add("DNAM", "技能，以及生命、魔法、耐力数值。");
				add("FACEDATA", "脸：发色、肤色、头部部件、变形和预设。");
				// ---- actor values ----
				add("Aggression", "AI：0 不主动攻击，1 有攻击性（见敌就攻击），2 极具攻击性，3 疯狂（攻击任何人）。");
				add("Confidence", "AI：0 胆小（总是逃跑）到 4 鲁莽（从不逃跑）。");
				add("Energy", "AI：角色在原地活动时走动的多少。");
				add("Morality", "AI：0 什么罪都肯犯，到 3 任何坏事都不做：它愿意为你做什么。");
				add("Mood", "AI：它表现出的面部情绪（中性、愤怒、开心……）。");
				add("Assistance", "AI：0 谁都不帮，1 帮盟友，2 战斗中帮朋友和盟友。");
				add("Magicka", "魔法：施法消耗掉的部分。");
				add("Stamina", "耐力：冲刺、重击和盾击消耗掉的部分。");
				add("HealRate", "脱离战斗后每秒恢复的生命，按最大值的百分比算。");
				add("MagickaRate", "每秒恢复的魔法，按最大值的百分比算。");
				add("StaminaRate", "每秒恢复的耐力，按最大值的百分比算。");
				add("SpeedMult", "移动速度，按百分比（100 = 正常）。异常的值会让你极慢或极快。");
				add("InventoryWeight", "你身上物品的重量（游戏会重新计算）。");
				add("CarryWeight", "在超重之前你能携带多少。");
				add("CriticalChance", "打出暴击的几率，按百分比。");
				add("MeleeDamage", "额外的近战伤害。");
				add("UnarmedDamage", "徒手（拳头）攻击的伤害。");
				add("Mass", "物理质量（有多难被推开）。");
				add("VoicePoints", "龙吼的威力。");
				add("VoiceRate", "龙吼恢复得多快。");
				add("DamageResist", "护甲值。");
				add("PoisonResist", "毒素抗性，按百分比。");
				add("ResistFire", "火焰抗性，按百分比。");
				add("ResistShock", "闪电抗性，按百分比。");
				add("ResistFrost", "冰霜抗性，按百分比。");
				add("ResistMagic", "全系魔法抗性，按百分比。");
				add("ResistDisease", "疾病抗性，按百分比。");
				add("Paralysis", "麻痹时大于 0。卡住的值会让你一直僵住。");
				add("Invisibility", "隐形时大于 0。卡住的值会让你一直隐形。");
				add("NightEye", "夜视生效时大于 0。");
				add("DetectLifeRange", "探测生命的范围。");
				add("WaterBreathing", "能在水下呼吸时大于 0。");
				add("WaterWalking", "能在水面上行走时大于 0。");
				add("Fame", "声望：游戏记下的善行。");
				add("Infamy", "恶名：游戏记下的恶行。");
				add("JumpingBonus", "额外的跳跃高度。");
				add("WardPower", "你当前结界术的强度。");
				add("ArmorPerks", "专长带来的护甲值加成，按百分比。");
				add("ShieldPerks", "专长带来的盾牌加成，按百分比。");
				add("BowSpeedBonus", "弓的拉弓速度加成（瞄准时时间变慢）。");
				add("WeaponSpeedMult", "武器挥动速度倍率（右手）。0 或 1 = 正常。");
				add("LeftWeaponSpeedMultiply", "武器挥动速度倍率（左手）。");
				add("ShoutRecoveryMult", "龙吼冷却倍率（1 = 正常，越小越快）。");
				add("MovementNoiseMult", "你移动时发出的噪音大小（1 = 正常）。");
				add("DragonSouls", "还没用来解锁龙吼词语的龙魂。");
				add("CombatHealthRegenMultiply", "战斗中的生命恢复（原版玩家为 0.7）。");
				add("Variable01", "游戏和模组自己拿来用的一个备用值。");
				add("WaitingForPlayer", "随从在等你时设置。");
				// ---- extra data ----
				add("Ownership", "谁拥有它（某个 NPC 或派系）：拿走就算是偷窃。");
				add("EncounterZone", "它的遭遇区域：刷出来的东西的等级范围，以及什么时候重置。");
				add("AliasInstanceArray", "它填充的任务别名：指向它的那些任务。删掉这些可能弄坏任务。");
				add("LeveledCreature", "这个角色来自哪个等级列表，以及抽出来的那个角色。");
				add("PromotedRef", "它为什么是持久化的：那些指向它的东西。");
				add("Package", "它正在运行的 AI 包。");
				add("PackageData", "正在运行的 AI 包的数据。");
				add("PackageStartLocation", "它当前的 AI 包是从哪里开始的。");
				add("RunOncePacks", "它已经运行过的「只运行一次」AI 包。");
				add("SceneData", "它所在的场景。");
				add("FactionChanges", "游戏过程中它加入或退出的派系，以及阶位变化。");
				add("DismemberedLimbs", "它失去的肢体。");
				add("AttachedArrows3D", "插在它身上的箭。");
				add("Ghost", "它是幽灵（攻击会穿透它）。");
				add("TextDisplayData", "游戏过程中设置的名字。");
				add("CanTalkToPlayer", "它能不能和你说话。");
				add("FollowerSwimBreadcrumbs", "它在水里跟随队长走过的路径痕迹。");
				add("GuardedRefData", "它记得自己看守过什么。");
				add("PlayerCrimeList", "它看见你犯下的罪行。");
				add("ForcedTarget", "它被强制攻击的目标。");
				add("HeadingTarget", "它被要求转向的目标。");
				add("ForcedLandingMarker", "它（通常是龙）被强制降落的地点。");
				add("FriendHits", "它还是友好状态时，你打了它多少次。");
				add("CachedScale", "它的大小（缓存值）。");
				add("ActorCause", "是什么创造了它（例如召唤它的法术）。");
				add("AshPileRef", "它留下的灰堆（幽灵、吸血鬼……）。");
				add("StartingWorldOrCell", "它一开始所在的世界或单元格。");
				add("Worn", "由角色穿戴。");
				add("Count", "这堆物品有几个。");
				add("Charge", "剩余的附魔充能。");
				add("Health", "对角色：它的生命值。对物品：它的耐久（强化程度，1.0 = 正常）。");
				add("Enchantment", "游戏过程中加上的附魔（你自己附的魔）。");
				add("LocRef", "用一个引用和一个半径给出的地点。");
				add("LocPt", "用单元格或世界中的一点给出的地点。");
				add("LocCell", "用一个单元格给出的地点。");
				return m;
			}();
			return g;
		}
	}

	const char* Explain(std::string_view a_label)
	{
		const auto& g = Glossary();
		if (const auto it = g.find(Key(a_label)); it != g.end()) {
			return it->second;
		}
		return nullptr;
	}
}

#include "skin_sdk.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "../../utils/memory/patternscan/patternscan.h"
#include "../../utils/memory/Interface/Interface.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/memory/vfunc/vfunc.h"
#include "../../utils/console/console.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/schema/schema.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/sdk/CUtlSymbolLarge.h"

namespace
{
	using FnVoid = void(__fastcall*)();
	using FnCall = void*(__fastcall*)();
	using FnEconSys = void*(__fastcall*)(void*);
	using FnInvGet = void*(__fastcall*)();
	using FnStaticData = CEconItemDefinition*(__fastcall*)(C_EconItemView*);
	using FnSetAttr = void(__fastcall*)(C_EconItemView*, const char*, float);
	using FnSetModel = void(__fastcall*)(C_BaseEntity*, const char*);
	using FnSetMask = void(__fastcall*)(CGameSceneNode*, uint64_t);
	using FnUpdateSubclass = void(__fastcall*)(C_CSWeaponBase*);
	using FnUpdateSkin = void(__fastcall*)(C_CSWeaponBase*, bool);
	using FnUpdateComp = void(__fastcall*)(void*, bool);
	using FnUpdateCompSet = void(__fastcall*)(C_CSWeaponBase*, bool);
	using FnSetBody = void(__fastcall*)(C_CSPlayerPawn*, const char*, int);
	using FnUpdateBody = void(__fastcall*)(C_CSPlayerPawn*);
	using FnFindHud = void*(__fastcall*)(const char*);
	using FnClearHud = int64_t(__fastcall*)(void*, int, int64_t);
	using FnUpdateRows = void(__fastcall*)(void*);
	using FnLocalize = const char*(__fastcall*)(void*, const char*);
	using FnSetSkeletonModel = void(__fastcall*)(CGameSceneNode*, void*);

	void* g_client = nullptr;
	void* g_inventory = nullptr;
	void* g_fs = nullptr;
	void* g_localize = nullptr;
	void* g_pResourceSystem = nullptr;
	FnSetSkeletonModel g_setSkeletonModel = nullptr;
	FnEconSys g_getEcon = nullptr;
	FnInvGet g_invGet = nullptr;
	FnStaticData g_getStatic = nullptr;
	FnSetAttr g_setAttr = nullptr;
	FnSetModel g_setModel = nullptr;
	FnSetMask g_setMask = nullptr;
	FnUpdateSubclass g_updateSubclass = nullptr;
	FnUpdateSkin g_updateSkin = nullptr;
	FnUpdateComp g_updateComp = nullptr;
	FnUpdateCompSet g_updateCompSet = nullptr;
	FnSetBody g_setBody = nullptr;
	FnUpdateBody g_updateBody = nullptr;
	FnFindHud g_findHud = nullptr;
	FnClearHud g_clearHud = nullptr;
	FnUpdateRows g_updateRows = nullptr;
	FnLocalize g_findSafe = nullptr;
	bool g_inited = false;

	void* Rel32(void* insn)
	{
		if (!insn)
			return nullptr;
		return M::GetAbsoluteAddress(reinterpret_cast<uint8_t*>(insn), 1);
	}

	void* ScanClient(const char* pat)
	{
		return M::FindPattern("client.dll", pat);
	}

	void* ScanCall(const char* pat)
	{
		void* hit = ScanClient(pat);
		return Rel32(hit);
	}

	void* ScanMod(const char* mod, const char* pat)
	{
		return M::FindPattern(mod, pat);
	}

	struct KnifeRow { int def; const char* weapon; };
	static const KnifeRow kKnives[] = {
		{ 500, "weapon_bayonet" },
		{ 503, "weapon_knife_css" },
		{ 505, "weapon_knife_flip" },
		{ 506, "weapon_knife_gut" },
		{ 507, "weapon_knife_karambit" },
		{ 508, "weapon_knife_m9_bayonet" },
		{ 509, "weapon_knife_tactical" },
		{ 512, "weapon_knife_falchion" },
		{ 514, "weapon_knife_survival_bowie" },
		{ 515, "weapon_knife_butterfly" },
		{ 516, "weapon_knife_push" },
		{ 517, "weapon_knife_cord" },
		{ 518, "weapon_knife_canis" },
		{ 519, "weapon_knife_ursus" },
		{ 520, "weapon_knife_gypsy_jackknife" },
		{ 521, "weapon_knife_outdoor" },
		{ 522, "weapon_knife_stiletto" },
		{ 523, "weapon_knife_widowmaker" },
		{ 525, "weapon_knife_skeleton" },
		{ 526, "weapon_knife_kukri" },
	};
}

uint16_t CEconItemDefinition::DefIndex() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(this) + 0x10);
}
uint8_t CEconItemDefinition::Rarity() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(this) + 0x42);
}
const char* CEconItemDefinition::ItemBaseName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x70);
}
const char* CEconItemDefinition::ItemTypeName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x80);
}
const char* SkinSdk::GetKnifeModelPath(uint16_t defIndex)
{
	switch (defIndex) {
	case 500: return "weapons/models/knife/knife_bayonet.vmdl";
	case 503: return "weapons/models/knife/knife_css.vmdl";
	case 505: return "weapons/models/knife/knife_flip.vmdl";
	case 506: return "weapons/models/knife/knife_gut.vmdl";
	case 507: return "weapons/models/knife/knife_karambit.vmdl";
	case 508: return "weapons/models/knife/knife_m9_bayonet.vmdl";
	case 509: return "weapons/models/knife/knife_tactical.vmdl";
	case 512: return "weapons/models/knife/knife_falchion.vmdl";
	case 514: return "weapons/models/knife/knife_survival_bowie.vmdl";
	case 515: return "weapons/models/knife/knife_butterfly.vmdl";
	case 516: return "weapons/models/knife/knife_push.vmdl";
	case 517: return "weapons/models/knife/knife_cord.vmdl";
	case 518: return "weapons/models/knife/knife_canis.vmdl";
	case 519: return "weapons/models/knife/knife_ursus.vmdl";
	case 520: return "weapons/models/knife/knife_gypsy_jackknife.vmdl";
	case 521: return "weapons/models/knife/knife_outdoor.vmdl";
	case 522: return "weapons/models/knife/knife_stiletto.vmdl";
	case 523: return "weapons/models/knife/knife_widowmaker.vmdl";
	case 525: return "weapons/models/knife/knife_skeleton.vmdl";
	case 526: return "weapons/models/knife/knife_kukri.vmdl";
	case 42:  return "weapons/models/knife/knife_default_ct.vmdl";
	case 59:  return "weapons/models/knife/knife_default_t.vmdl";
	default:  return nullptr;
	}
}

const char* CEconItemDefinition::ModelName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	const uint16_t def = DefIndex();
	const char* knifeModel = SkinSdk::GetKnifeModelPath(def);
	if (knifeModel)
		return knifeModel;
	const char* m = nullptr;
	__try { m = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x148); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (m && m[0] && strstr(m, ".vmdl")) return m;
	__try { m = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0xD8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (m && m[0] && strstr(m, ".vmdl")) return m;
	__try { m = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x1B8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	if (m && m[0] && strstr(m, ".vmdl")) return m;
	return nullptr;
}
int32_t CEconItemDefinition::StickerSupportCount() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + 0x168);
}
const char* CEconItemDefinition::IconName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x230);
}
const char* CEconItemDefinition::WeaponName() const
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(this) + 0x260);
}

bool CEconItemDefinition::IsKnife(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#CSGO_Type_Knife"))
		return false;
	return excludeDefault ? DefIndex() >= 500 : true;
}
bool CEconItemDefinition::IsGlove(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#Type_Hands"))
		return false;
	const bool def = DefIndex() == 5028;
	return excludeDefault ? !def : true;
}
bool CEconItemDefinition::IsAgent(bool excludeDefault) const
{
	const char* t = ItemTypeName();
	if (!t || hash_32_fnv1a_const(t) != hash_32_fnv1a_const("#Type_CustomPlayer"))
		return false;
	const bool def = DefIndex() == 5036 || DefIndex() == 5037;
	return excludeDefault ? !def : true;
}
bool CEconItemDefinition::IsWeapon() const
{
	if (IsKnife(false) || IsGlove(false) || IsAgent(false))
		return false;
	return StickerSupportCount() >= 4;
}

uint8_t CPaintKit::IsUseLegacyModel() const
{
	if (!this || !Mem::IsUserPtr(this)) return 0;
	return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(this) + 0xAE);
}

SkinUtlMap<int, CEconItemDefinition*>& CEconItemSchema::SortedItemDefinitionMap()
{
	return *reinterpret_cast<SkinUtlMap<int, CEconItemDefinition*>*>(
		reinterpret_cast<uint8_t*>(this) + 0x128);
}
SkinUtlMap<int, CPaintKit*>& CEconItemSchema::PaintKits()
{
	return *reinterpret_cast<SkinUtlMap<int, CPaintKit*>*>(
		reinterpret_cast<uint8_t*>(this) + 0x2F0);
}
CEconItemSchema* CEconItemSystem::Schema()
{
	if (!this || !Mem::IsUserPtr(this)) return nullptr;
	return *reinterpret_cast<CEconItemSchema**>(reinterpret_cast<uint8_t*>(this) + 0x8);
}

void SkinSdk::Init()
{
	if (g_inited)
		return;
	g_inited = true;

	g_client = I::Get<void>("client.dll", "Source2Client00");
	// Andromeda's exact GetEconItemSystem pattern (Source2Client)
	g_getEcon = reinterpret_cast<FnEconSys>(ScanClient(
		"48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81"));
	// Fallback via interface vtable if pattern miss (never happens but safe)
	if (!g_getEcon) {
		Con::PatternMiss("client", "GetEconItemSystem");
	}
	g_invGet = reinterpret_cast<FnInvGet>(ScanCall(
		"E8 ? ? ? ? 48 8B D8 E8 ? ? ? ? 8B 70"));
	// Andromeda's full C_EconItemView_GetStaticData (prefix + full tail) - use full for stability
	g_getStatic = reinterpret_cast<FnStaticData>(ScanClient(
		"40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9 ? ? ? ? 48 89 7C 24 ? E8 ? ? ? ? 33 FF 48 8B D8 48 85 C0 74 ? 48 8D 05 ? ? ? ? 48 89 7B ? B9 ? ? ? ? 48 89 03 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 E8 ? ? ? ? 48 8B F8 48 8D 05 ? ? ? ? 48 89 7B ? 48 89 03 EB ? 48 8B DF 48 8B 7C 24 ? 48 89 1D ? ? ? ? 48 8B 4B ? 48 8B 5C 24 ? 48 85 C9 75"));
	if (!g_getStatic) {
		g_getStatic = reinterpret_cast<FnStaticData>(ScanClient(
			"40 56 48 83 EC ? 48 89 5C 24 ? 48 8B F1 48 8B 1D ? ? ? ? 48 85 DB 75 ? B9"));
		if (!g_getStatic) Con::PatternMiss("client", "GetStaticData");
	}
	g_setAttr = reinterpret_cast<FnSetAttr>(ScanCall(
		"E8 ? ? ? ? 66 41 0F 6E D4"));
	if (!g_setAttr) Con::PatternMiss("client", "SetAttributeValueByName");
	g_setModel = reinterpret_cast<FnSetModel>(ScanClient(
		"40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40"));
	if (!g_setModel) Con::PatternMiss("client", "SetModel");
	g_setMask = reinterpret_cast<FnSetMask>(ScanClient(
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71"));
	if (!g_setMask) Con::PatternMiss("client", "SetMeshGroupMask");
	g_updateSubclass = reinterpret_cast<FnUpdateSubclass>(ScanClient(
		"4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41"));
	if (!g_updateSubclass) Con::PatternMiss("client", "UpdateSubclass");
	// Andromeda's UpdateSkin full pattern + short fallback
	g_updateSkin = reinterpret_cast<FnUpdateSkin>(ScanClient(
		"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A 33 D2 48 8B CF E8 ? ? ? ? 48 8D 8F 90 19 00 00"));
	if (!g_updateSkin)
		g_updateSkin = reinterpret_cast<FnUpdateSkin>(ScanClient(
			"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 0A"));
	if (!g_updateSkin) Con::PatternMiss("client", "UpdateSkin");
	// Composite: prefer Andromeda's CALL pattern (more stable across builds) then direct
	g_updateComp = reinterpret_cast<FnUpdateComp>(ScanCall(
		"E8 ? ? ? ? 48 8D 8B ? ? ? ? 48 89 BC 24"));
	if (!g_updateComp)
		g_updateComp = reinterpret_cast<FnUpdateComp>(ScanClient(
			"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 44 0F B6 F2"));
	if (!g_updateComp) Con::PatternMiss("client", "UpdateCompositeMaterial");
	g_updateCompSet = reinterpret_cast<FnUpdateCompSet>(ScanClient(
		"40 55 53 41 57 48 8D AC 24 00 FE ? ?"));
	if (!g_updateCompSet) Con::PatternMiss("client", "UpdateCompositeMaterialSet");
	g_setBody = reinterpret_cast<FnSetBody>(ScanCall("E8 ? ? ? ? EB 0C 48 8B CF"));
	if (!g_setBody) Con::PatternMiss("client", "SetBodyGroup");
	g_updateBody = reinterpret_cast<FnUpdateBody>(ScanCall(
		"E8 ? ? ? ? 48 8B 9C 24 ? ? ? ? 4C 8B B4 24 ? ? ? ? 48 83 C4"));
	if (!g_updateBody) Con::PatternMiss("client", "UpdateBodyGroupChoice");
	g_findHud = reinterpret_cast<FnFindHud>(ScanClient(
		"40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24 ? 48 8D 88 58 02 00 00"));
	if (!g_findHud) Con::PatternMiss("client", "FindHudElement");
	g_clearHud = reinterpret_cast<FnClearHud>(ScanCall("E8 ? ? ? ? 8B F8 C6 84 24"));
	if (!g_clearHud) Con::PatternMiss("client", "ClearHudWeaponIcon");
	g_updateRows = reinterpret_cast<FnUpdateRows>(ScanClient(
		"48 89 5C 24 10 57 48 83 EC 30 33 FF 48 8B D9 39 79 38"));
	if (!g_updateRows) Con::PatternMiss("client", "UpdateWeaponRows");
	g_findSafe = reinterpret_cast<FnLocalize>(ScanMod("localize.dll",
		"40 56 57 48 83 EC ? 48 8B F2 48 8B F9 48 85 D2 0F 84"));
	if (!g_findSafe) Con::Warn("SkinSdk localize FindSafe miss (non-fatal)");

	void* pScanRes = ScanClient("4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24 40");
	if (pScanRes) {
		uint8_t* pRel = reinterpret_cast<uint8_t*>(pScanRes) + 3;
		void** ppRes = reinterpret_cast<void**>(M::GetAbsoluteAddress(pRel, 3, 0));
		if (ppRes && Mem::IsUserPtr(ppRes))
			g_pResourceSystem = *ppRes;
	}
	if (!g_pResourceSystem) {
		g_pResourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem001");
		if (!g_pResourceSystem)
			g_pResourceSystem = I::Get<void>("resourcesystem", "ResourceSystem001");
	}
	if (!g_pResourceSystem) Con::PatternMiss("client", "ResourceSystem");

	g_setSkeletonModel = reinterpret_cast<FnSetSkeletonModel>(ScanClient(
		"53 48 83 EC 20 48 8B 49 08 48 8B DA 48 8B 01 FF 50 70"));
	if (!g_setSkeletonModel) Con::PatternMiss("client", "CSkeletonInstance::SetModel");

	g_fs = I::Get<void>("filesystem_stdio.dll", "VFileSystem017");
	if (!g_fs)
		g_fs = I::Get<void>("filesystem_stdio", "VFileSystem017");
	g_localize = I::Get<void>("localize.dll", "Localize_001");
	if (!g_localize)
		g_localize = I::Get<void>("localize", "Localize_001");

	// Detailed ok/warn per critical weapon path
	if (!g_getEcon || !g_getStatic || !g_setAttr || !g_setModel || !g_updateSkin)
		Con::Error("SkinSdk critical miss: econ=%p static=%p attr=%p setModel=%p skin=%p (weapon knife will fail)", (void*)g_getEcon, (void*)g_getStatic, (void*)g_setAttr, (void*)g_setModel, (void*)g_updateSkin);
	else
		Con::Ok("SkinSdk client=%p econ=%p invGet=%p static=%p attr=%p setModel=%p mask=%p skin=%p comp=%p compSet=%p hud=%p fs=%p loc=%p",
			g_client, (void*)g_getEcon, (void*)g_invGet, (void*)g_getStatic, (void*)g_setAttr,
			(void*)g_setModel, (void*)g_setMask, (void*)g_updateSkin, (void*)g_updateComp, (void*)g_updateCompSet, (void*)g_findHud, g_fs, g_localize);
}

void* SkinSdk::Source2Client()
{
	if (!g_client)
		g_client = I::Get<void>("client.dll", "Source2Client00");
	return g_client;
}

CEconItemSystem* SkinSdk::EconItemSystem()
{
	void* c = Source2Client();
	if (!c || !g_getEcon)
		return nullptr;
	void* sys = nullptr;
	__try { sys = g_getEcon(c); }
	__except (EXCEPTION_EXECUTE_HANDLER) { sys = nullptr; }
	return reinterpret_cast<CEconItemSystem*>(sys);
}

CEconItemSchema* SkinSdk::EconSchema()
{
	CEconItemSystem* sys = EconItemSystem();
	if (!sys || !Mem::IsUserPtr(sys))
		return nullptr;
	CEconItemSchema* s = nullptr;
	__try { s = sys->Schema(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { s = nullptr; }
	return (s && Mem::IsUserPtr(s)) ? s : nullptr;
}

void* SkinSdk::LocalInventory()
{
	if (!g_invGet)
		return nullptr;
	void* mgr = nullptr;
	__try { mgr = g_invGet(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { mgr = nullptr; }
	if (!mgr || !Mem::IsUserPtr(mgr))
		return nullptr;
	void* inv = nullptr;
	__try { inv = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(mgr) + 0x3F540); }
	__except (EXCEPTION_EXECUTE_HANDLER) { inv = nullptr; }
	return (inv && Mem::IsUserPtr(inv)) ? inv : nullptr;
}

uint64_t SkinSdk::InventorySteamId()
{
	void* inv = LocalInventory();
	if (!inv)
		return 0;
	uint64_t id = 0;
	__try { id = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(inv) + 0x10); }
	__except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
	return id;
}

void* SkinSdk::FileSystem() { return g_fs; }

bool SkinSdk::FileExistsGame(const char* path)
{
	if (!g_fs || !path || !path[0])
		return false;
	bool ok = false;
	__try { ok = M::vfunc<bool, 21U>(g_fs, path, "GAME"); }
	__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
	return ok;
}

const char* SkinSdk::LocalizeSafe(const char* token, const char* fallback)
{
	if (token && token[0] && g_localize && g_findSafe) {
		const char* loc = nullptr;
		__try { loc = g_findSafe(g_localize, token); }
		__except (EXCEPTION_EXECUTE_HANDLER) { loc = nullptr; }
		if (loc && loc[0] && loc[0] != '#')
			return loc;
	}
	return (fallback && fallback[0]) ? fallback : "Unknown";
}

CEconItemDefinition* SkinSdk::FindDefByIndex(uint16_t defIdx)
{
	CEconItemSchema* schema = EconSchema();
	if (!schema)
		return nullptr;
	auto& map = schema->SortedItemDefinitionMap();
	if (!map.m_data || map.m_size <= 0 || map.m_size > 20000)
		return nullptr;
	for (int i = 0; i < map.m_size; ++i) {
		CEconItemDefinition* d = map.m_data[i].m_value;
		if (d && Mem::IsUserPtr(d) && d->DefIndex() == defIdx)
			return d;
	}
	return nullptr;
}

CEconItemDefinition* SkinSdk::GetStaticData(C_EconItemView* view)
{
	if (!view || !g_getStatic)
		return nullptr;
	CEconItemDefinition* d = nullptr;
	__try { d = g_getStatic(view); }
	__except (EXCEPTION_EXECUTE_HANDLER) { d = nullptr; }
	return (d && Mem::IsUserPtr(d)) ? d : nullptr;
}

void SkinSdk::SetAttributeValueByName(C_EconItemView* view, const char* name, float value)
{
	if (!view || !name || !g_setAttr)
		return;
	__try { g_setAttr(view, name, value); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::SetModel(C_BaseEntity* ent, const char* model)
{
	if (!ent || !model || !model[0] || !g_setModel || !Mem::ValidEntity(ent))
		return;

	// g_setModel is C_CSPlayerPawn::SetModel — only safe for alive player pawns (Agent Changer).
	// Calling it on weapons, viewmodels, props, or dead/dying pawns corrupts entity physics memory.
	if (!ent->IsBasePlayer())
		return;

	int hp = 0;
	uint8_t life = 0;
	__try {
		hp = reinterpret_cast<C_CSPlayerPawn*>(ent)->m_iHealth();
		life = reinterpret_cast<C_CSPlayerPawn*>(ent)->m_lifeState();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (hp <= 0 || life != 0)
		return;

	// Deduplicate: If the entity already has this model loaded, do not call g_setModel again.
	// Repeatedly calling SetModel destroys and recreates the scene node / skeleton, causing worker thread race crashes.
	CGameSceneNode* node = ent->m_pGameSceneNode();
	if (node && Mem::ValidEntity(node)) {
		__try {
			const char* curModel = node->m_modelState().m_ModelName().String();
			if (curModel && curModel[0] && _stricmp(curModel, model) == 0)
				return;
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	__try { g_setModel(ent, model); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::SetSceneNodeModel(CGameSceneNode* node, const char* modelPath)
{
	if (!node || !modelPath || !modelPath[0] || !Mem::ValidEntity(node))
		return;

	if (!g_pResourceSystem) {
		g_pResourceSystem = I::Get<void>("resourcesystem.dll", "ResourceSystem001");
		if (!g_pResourceSystem)
			g_pResourceSystem = I::Get<void>("resourcesystem", "ResourceSystem001");
	}

	void* modelHandle = nullptr;
	if (g_pResourceSystem && Mem::IsUserPtr(g_pResourceSystem)) {
		__try {
			M::vfunc<void*, 12U>(g_pResourceSystem, &modelHandle, modelPath);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			modelHandle = nullptr;
		}
	}

	if (modelHandle && g_setSkeletonModel) {
		__try {
			g_setSkeletonModel(node, modelHandle);
		} __except (EXCEPTION_EXECUTE_HANDLER) {}
	}
}

void SkinSdk::SetMeshGroupMask(CGameSceneNode* node, uint64_t mask)
{
	if (!node || !g_setMask)
		return;
	__try { g_setMask(node, mask); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::PostDataUpdate(CGameSceneNode* node)
{
	// No-op: CGameSceneNode does not have PostDataUpdate. Calling vfunc<25U> corrupts node bounds flags.
	(void)node;
}

void SkinSdk::UpdateSubclass(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateSubclass)
		return;
	__try { g_updateSubclass(weapon); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateSkin(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateSkin)
		return;
	__try { g_updateSkin(weapon, true); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateCompositeMaterial(void* compositeOwner)
{
	if (!compositeOwner || !g_updateComp)
		return;
	__try { g_updateComp(compositeOwner, true); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateCompositeMaterialSet(C_CSWeaponBase* weapon)
{
	if (!weapon || !g_updateCompSet)
		return;
	__try { g_updateCompSet(weapon, false); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::SetBodyGroup(C_CSPlayerPawn* pawn)
{
	if (!pawn || !g_setBody)
		return;
	__try { g_setBody(pawn, "first_or_third_person", 1); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SkinSdk::UpdateBodyGroupChoice(C_CSPlayerPawn* pawn)
{
	if (!pawn || !g_updateBody)
		return;
	__try { g_updateBody(pawn); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* SkinSdk::FindHudElement(const char* name)
{
	if (!name || !g_findHud)
		return nullptr;
	void* h = nullptr;
	__try { h = g_findHud(name); }
	__except (EXCEPTION_EXECUTE_HANDLER) { h = nullptr; }
	return h;
}

int SkinSdk::ClearHudWeaponIcon(void* hudWeapons, int slot, int64_t unk)
{
	if (!hudWeapons || !g_clearHud)
		return -1;
	int64_t r = -1;
	__try { r = g_clearHud(hudWeapons, slot, unk); }
	__except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
	return static_cast<int>(r);
}

void SkinSdk::UpdateWeaponRows(void* hudWeapons)
{
	if (!hudWeapons || !g_updateRows)
		return;
	__try { g_updateRows(hudWeapons); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* SkinSdk::CompositeOwner(C_CSWeaponBase* weapon)
{
	if (!weapon)
		return nullptr;
	return reinterpret_cast<uint8_t*>(weapon) + 0x608;
}

static C_BaseEntity* HudChildByName(C_CSPlayerPawn* pawn, const char* needle)
{
	if (!pawn)
		return nullptr;
	static uint32_t s_hud = 0;
	if (!s_hud)
		s_hud = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_hHudModelArms"));
	if (!s_hud || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	CBaseHandle h{};
	__try { h = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uint8_t*>(pawn) + s_hud); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!h.valid())
		return nullptr;
	// Index-only resolve: serial-checked Get(CBaseHandle) can reject hud-model
	// handles whose stored serial carries the flags bit. Matches Andromeda.
	C_BaseEntity* arms = (I::GameEntity && I::GameEntity->Instance)
		? I::GameEntity->Instance->Get<C_BaseEntity>(h.index()) : nullptr;
	if (!arms)
		return nullptr;
	CGameSceneNode* node = arms->m_pGameSceneNode();
	if (!node)
		return nullptr;
	for (CGameSceneNode* child = node->m_pChild(); child; child = child->m_pNextSibling()) {
		CEntityInstance* owner = child->m_pOwner();
		if (!owner || !Mem::IsUserPtr(owner))
			continue;
		auto* ent = reinterpret_cast<C_BaseEntity*>(owner);
		if (!ent->IsViewmodel())
			continue;
		if (!needle)
			return ent;
		CGameSceneNode* n = ent->m_pGameSceneNode();
		if (!n)
			continue;
		const char* mn = n->m_modelState().m_ModelName().String();
		if (mn && strstr(mn, needle))
			return ent;
	}
	return nullptr;
}

// Andromeda semantics: viewmodel's m_hOwnerEntity resolves to the owning
// weapon entity. Engine stores hud-model handles with serial that may carry
// the flags bit (CEntityInstance::handle() strips it) — resolve by INDEX
// only, like Andromeda's CHandle::Get (GetBaseEntity(GetEntryIndex())).
static C_BaseEntity* VmOwnerEntity(C_BaseEntity* ent)
{
	if (!ent || !ent->m_hOwnerEntity().valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	return I::GameEntity->Instance->Get<C_BaseEntity>(ent->m_hOwnerEntity().index());
}

C_BaseEntity* SkinSdk::GetViewModel(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return nullptr;
	// Andromeda GetLocalActiveWeapon: m_hActiveWeapon INDEX-only resolve —
	// serial-checked Get can reject live weapon handles (flags bit in serial).
	C_CSWeaponBase* wpn = nullptr;
	if (CCSPlayer_WeaponServices* sws = pawn->GetWeaponServices(); sws) {
		const CBaseHandle ha = sws->m_hActiveWeapon();
		if (ha.valid() && I::GameEntity && I::GameEntity->Instance)
			wpn = I::GameEntity->Instance->Get<C_CSWeaponBase>(ha.index());
	}
	if (!wpn)
		wpn = pawn->GetActiveWeapon();
	if (!wpn)
		return HudChildByName(pawn, nullptr);
	static uint32_t s_hud = 0;
	if (!s_hud)
		s_hud = SchemaFinder::Get(hash_32_fnv1a_const("C_CSPlayerPawn->m_hHudModelArms"));
	if (!s_hud || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
CBaseHandle h{};
	__try { h = *reinterpret_cast<CBaseHandle*>(reinterpret_cast<uint8_t*>(pawn) + s_hud); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance)
		return nullptr;
	C_BaseEntity* arms = I::GameEntity->Instance->Get<C_BaseEntity>(h.index());
	if (!arms)
		return nullptr;
	CGameSceneNode* node = arms->m_pGameSceneNode();
	if (!node)
		return nullptr;
	for (CGameSceneNode* child = node->m_pChild(); child; child = child->m_pNextSibling()) {
		CEntityInstance* owner = child->m_pOwner();
		if (!owner || !Mem::IsUserPtr(owner))
			continue;
		auto* ent = reinterpret_cast<C_BaseEntity*>(owner);
		if (!ent->IsViewmodel())
			continue;
		if (VmOwnerEntity(ent) == reinterpret_cast<C_BaseEntity*>(wpn))
			return ent;
	}
	return nullptr;
}

C_BaseEntity* SkinSdk::GetKnifeModel(C_CSPlayerPawn* pawn)
{
	return HudChildByName(pawn, "knife");
}

const char* SkinSdk::KnifeWeaponName(int defIdx)
{
	for (const auto& k : kKnives)
		if (k.def == defIdx)
			return k.weapon;
	return "";
}

const char* SkinSdk::KnifeIconName(int defIdx)
{
	const char* w = KnifeWeaponName(defIdx);
	if (w && strncmp(w, "weapon_", 7) == 0)
		return w + 7;
	return w ? w : "";
}

bool SkinSdk::IsSkinnableGloveDef(int defIdx)
{
	switch (defIdx) {
	case kGloveBloodhound:
	case kGloveBrokenFang:
	case kGloveSporty:
	case kGloveSlick:
	case kGloveHandwraps:
	case kGloveMotorcycle:
	case kGloveSpecialist:
	case kGloveHydra:
		return true;
	default:
		return false;
	}
}

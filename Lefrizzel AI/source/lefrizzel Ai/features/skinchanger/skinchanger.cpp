#include "skinchanger.h"
#include "skin_sdk.h"
#include "skin_items.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "../../config/config.h"
#include "../../hooks/hooks.h"
#include "../../interfaces/interfaces.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/memsafe/memsafe.h"
#include "../../utils/console/console.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_CSWeaponBase/C_CSWeaponBase.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"
#include "../../../cs2/entity/CCSPlayerController/CCSPlayerController.h"
#include "../../../cs2/sdk/IGameEvent.h"

namespace
{
	// SEH helpers: object-free so they can use __try (C2712 otherwise)
	static CBaseHandle SehReadHandle(void* p) {
		__try { return *reinterpret_cast<CBaseHandle*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return CBaseHandle{}; }
	}
	static int SehReadInt(void* p) {
		__try { return *reinterpret_cast<int*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	static CBaseHandle* SehReadPtr(void* p) {
		__try { return *reinterpret_cast<CBaseHandle**>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}
	static float SehReadFloat(void* p) {
		__try { return *reinterpret_cast<float*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
	}
	struct SkinCfg {
		uint16_t def = 0;
		int paint = 0;
		float wear = 0.f;
		int seed = 0;
		bool enabled = false;
		bool legacy = false;
		bool stattrak = false;
		int stattrakCount = 0;
	};

	bool g_forceReapply = false;
	bool g_applyGloves = false;
	uint64_t g_agentHash = 0;
	float g_lastSpawn = -1.f;
	C_BaseEntity* g_lastVm = nullptr;
	std::unordered_map<uint32_t, uint64_t> g_appliedSig;
	static int s_pendingHudClear = 0; // retry HUD clear for a few frames after skin apply / map join (HUD not yet created)
	static int s_spawnReapply = 0; // keep knife correct for ~24 frames after spawn — fixes 3s bugged model/anim

	uint32_t Off(const char* field)
	{
		return SchemaFinder::Get(hash_32_fnv1a_const(field));
	}

	template <typename T>
	T& Field(void* base, uint32_t off)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
	}

	C_EconItemView* ItemView(C_CSWeaponBase* w)
	{
		const uint32_t attr = Off("C_EconEntity->m_AttributeManager");
		const uint32_t item = Off("C_AttributeContainer->m_Item");
		if (!attr || !item)
			return nullptr;
		return reinterpret_cast<C_EconItemView*>(reinterpret_cast<uint8_t*>(w) + attr + item);
	}

	C_EconItemView* GloveView(C_CSPlayerPawn* pawn)
	{
		const uint32_t off = Off("C_CSPlayerPawn->m_EconGloves");
		if (!off)
			return nullptr;
		return reinterpret_cast<C_EconItemView*>(reinterpret_cast<uint8_t*>(pawn) + off);
	}

	void SetViewU16(C_EconItemView* v, const char* f, uint16_t val)
	{
		const uint32_t o = Off(f);
		if (o) Field<uint16_t>(v, o) = val;
	}
	void SetViewU32(C_EconItemView* v, const char* f, uint32_t val)
	{
		const uint32_t o = Off(f);
		if (o) Field<uint32_t>(v, o) = val;
	}
	void SetViewU64(C_EconItemView* v, const char* f, uint64_t val)
	{
		const uint32_t o = Off(f);
		if (o) Field<uint64_t>(v, o) = val;
	}
	void SetViewBool(C_EconItemView* v, const char* f, bool val)
	{
		const uint32_t o = Off(f);
		if (o) Field<bool>(v, o) = val;
	}
	uint16_t GetViewU16(C_EconItemView* v, const char* f)
	{
		const uint32_t o = Off(f);
		return o ? Field<uint16_t>(v, o) : 0;
	}

	void SetWeaponI32(C_CSWeaponBase* w, const char* f, int32_t val)
	{
		const uint32_t o = Off(f);
		if (o) Field<int32_t>(w, o) = val;
	}
	void SetWeaponU32(C_CSWeaponBase* w, const char* f, uint32_t val)
	{
		const uint32_t o = Off(f);
		if (o) Field<uint32_t>(w, o) = val;
	}
	void SetWeaponF32(C_CSWeaponBase* w, const char* f, float val)
	{
		const uint32_t o = Off(f);
		if (o) Field<float>(w, o) = val;
	}
	uint64_t OwnerXuid(C_CSWeaponBase* w)
	{
		const uint32_t lo = Off("C_EconEntity->m_OriginalOwnerXuidLow");
		const uint32_t hi = Off("C_EconEntity->m_OriginalOwnerXuidHigh");
		if (!lo || !hi) return 0;
		return (static_cast<uint64_t>(Field<uint32_t>(w, hi)) << 32) | Field<uint32_t>(w, lo);
	}

	float ClampWear(float w)
	{
		if (w < 0.0001f) return 0.0001f;
		if (w > 1.f) return 1.f;
		return w;
	}

	uint64_t MakeSig(const SkinCfg& cfg)
	{
		uint64_t s = cfg.def;
		s = (s << 16) ^ static_cast<uint64_t>(cfg.paint & 0xFFFF);
		s ^= (static_cast<uint64_t>(cfg.seed & 0xFFFF) << 32);
		s ^= cfg.legacy ? (1ull << 48) : 0;
		s ^= cfg.stattrak ? (1ull << 49) : 0;
		const uint32_t wearBits = *reinterpret_cast<const uint32_t*>(&cfg.wear);
		s ^= static_cast<uint64_t>(wearBits) << 16;
		return s;
	}

	bool IsGrenade(int id)
	{
		return id == 43 || id == 44 || id == 45 || id == 46 || id == 47 || id == 48 || id == 49;
	}
	bool IsDefaultKnife(int id) { return id == 42 || id == 59; }

	SkinCfg Resolve(uint16_t weaponDef, bool isKnife)
	{
		SkinCfg cfg;
		if (isKnife) {
			if (!Config::skin_knife || Config::skin_knife_def <= 0)
				return cfg;
			cfg.enabled = true;
			cfg.def = static_cast<uint16_t>(Config::skin_knife_def);
			cfg.paint = Config::skin_knife_paint;
			cfg.wear = Config::skin_knife_wear;
			cfg.seed = Config::skin_knife_seed;
			cfg.stattrak = Config::skin_knife_stattrak;
			return cfg;
		}
		auto it = Config::skin_weapons.find(weaponDef);
		if (it == Config::skin_weapons.end())
			return cfg;
		cfg.enabled = true;
		cfg.def = weaponDef;
		cfg.paint = it->second.paint;
		cfg.wear = it->second.wear;
		cfg.seed = it->second.seed;
		cfg.stattrak = it->second.stattrak;
		return cfg;
	}

	bool LookupLegacy(uint16_t def, int paint)
	{
		for (const auto& dumped : GetSkinItems().Items()) {
			if (dumped.def != def) continue;
			for (const auto& s : dumped.skins)
				if (s.id == paint) return s.legacy;
			break;
		}
		return false;
	}

	bool IsSkinnedWeaponId(int weaponId)
	{
		if (weaponId == 42 || weaponId == 59 || (weaponId >= 500 && weaponId <= 526))
			return Config::skin_knife;
		return Config::skin_weapons.find(weaponId) != Config::skin_weapons.end();
	}

	// Helper: get weapon definition index from handle (SEH-safe, for HUD switch check)
	static int GetWeaponDefFromHandleSafe(const CBaseHandle& h) {
		if (!h.valid() || !I::GameEntity || !I::GameEntity->Instance) return 0;
		__try {
			auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(h.index());
			if (!w || !Mem::ValidEntity(w)) return 0;
			C_EconItemView* view = ItemView(w);
			if (!view) return 0;
			return GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
		} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	static bool ClearHudIconSlots()
	{
		bool ok = false;
		__try {
			auto ClearMatched = [](const char* hudName, uintptr_t backOff) -> bool {
				void* pHud = SkinSdk::FindHudElement(hudName);
				if (!pHud) return false;
				auto* pHudWeapons = reinterpret_cast<uint8_t*>(pHud) - backOff;
				auto SlotCount = [](uint8_t* p) -> int {
					const int n = *reinterpret_cast<int*>(p + 0x50);
					return (n > 0 && n <= 64) ? n : 0;
				};
				int nCount = SlotCount(pHudWeapons);
				if (nCount <= 0) return false;
				auto* pSlots = *reinterpret_cast<uint8_t**>(pHudWeapons + 0x58);
				if (!pSlots) return false;
				int i = 0, guard = 0, cleared = 0;
				while (i >= 0 && i < nCount && ++guard <= 128) {
					nCount = SlotCount(pHudWeapons);
					if (nCount <= 0) break;
					const int weaponId = *reinterpret_cast<int*>(pSlots + (size_t)i * 72 + 60);
					if (!IsSkinnedWeaponId(weaponId)) { ++i; continue; }
					const int next = SkinSdk::ClearHudWeaponIcon(pHudWeapons, i, 0);
					i = next + 1;
					++cleared;
				}
				if (cleared)
					SkinSdk::UpdateWeaponRows(pHudWeapons);
				return true;
			};
			if (ClearMatched("HudWeaponSelection", 0x98)) ok = true;
			else if (ClearMatched("CCSGO_HudWeaponSelection", 0xA0)) ok = true;
		} __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		return ok;
	}

	uint64_t MeshMask(bool isKnife, bool legacy)
	{
		(void)isKnife;
		return legacy ? 2ull : 1ull;
	}

	void ApplyPaint(C_CSWeaponBase* w, C_EconItemView* view, const SkinCfg& cfg, uint32_t accountId, bool isKnife)
	{
		if (!w || !view || !cfg.enabled)
			return;
		const uint16_t applyDef = cfg.def ? cfg.def : GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
		const float wear = ClampWear(cfg.wear);
		const float seed = static_cast<float>(cfg.seed >= 0 ? cfg.seed : 0);
		// Andromeda: accountId from steamID, fallback 0 -> still set (SOC needs non-zero? use 1 if 0)
		if (!accountId) accountId = static_cast<uint32_t>(SkinSdk::InventorySteamId());
		if (!accountId) accountId = 1;

		SetViewBool(view, "C_EconItemView->m_bDisallowSOC", true);
		SetViewBool(view, "C_EconItemView->m_bRestoreCustomMaterialAfterPrecache", true);
		SetViewBool(view, "C_EconItemView->m_bInitialized", true);
		SetViewU32(view, "C_EconItemView->m_iAccountID", accountId);
		if (isKnife && cfg.def)
			SetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex", applyDef);
		// Andromeda sets both High and 64-bit ID (0xFFFFFFFF00000000). Also set Low for completeness (some builds read Low)
		SetViewU32(view, "C_EconItemView->m_iItemIDHigh", static_cast<uint32_t>(-1));
		SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
		SetViewU64(view, "C_EconItemView->m_iItemID",
			(static_cast<uint64_t>(static_cast<uint32_t>(-1)) << 32));

		// Dump fallbacks 0x1680/0x1684/0x1688 ensure schema miss still writes (schema may return 0 during early init)
		SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", cfg.paint);
		SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", cfg.seed);
		SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", wear);
		if (cfg.stattrak)
			SetWeaponI32(w, "C_EconEntity->m_nFallbackStatTrak", cfg.stattrakCount);
		// Fallback hard offsets if schema miss (Andromeda parity: direct 0x1680 etc never fails)
		if (!Off("C_EconEntity->m_nFallbackPaintKit"))
			*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1680) = cfg.paint;
		if (!Off("C_EconEntity->m_nFallbackSeed"))
			*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1684) = cfg.seed;
		if (!Off("C_EconEntity->m_flFallbackWear"))
			*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(w) + 0x1688) = wear;

		const uint64_t steamId = SkinSdk::InventorySteamId();
		const uint32_t xuidLow = static_cast<uint32_t>(steamId & 0xFFFFFFFF);
		const uint32_t xuidHigh = static_cast<uint32_t>((steamId >> 32) & 0xFFFFFFFF);
		SetWeaponU32(w, "C_EconEntity->m_OriginalOwnerXuidLow", xuidLow ? xuidLow : accountId);
		SetWeaponU32(w, "C_EconEntity->m_OriginalOwnerXuidHigh", xuidHigh);
		if (!Off("C_EconEntity->m_OriginalOwnerXuidLow"))
			*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1678) = xuidLow ? xuidLow : accountId;
		if (!Off("C_EconEntity->m_OriginalOwnerXuidHigh"))
			*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(w) + 0x167C) = xuidHigh;

		if (cfg.paint > 0) {
			SkinSdk::SetAttributeValueByName(view, "set item texture preference", static_cast<float>(cfg.paint));
			SkinSdk::SetAttributeValueByName(view, "set item texture prefab", static_cast<float>(cfg.paint));
		}
		SkinSdk::SetAttributeValueByName(view, "set item texture wear", wear);
		SkinSdk::SetAttributeValueByName(view, "set item texture seed", seed);
		if (cfg.stattrak) {
			SkinSdk::SetAttributeValueByName(view, "kill eater", static_cast<float>(cfg.stattrakCount));
			SkinSdk::SetAttributeValueByName(view, "kill eater score type", 0.f);
		}
	}

	void WalkWeapons(C_CSPlayerPawn* pawn, CCSPlayer_WeaponServices* ws, C_BaseEntity* vm,
		uint32_t accountId, uint64_t steamId, bool force, bool fullRefresh)
	{
		static uint32_t s_myWeapons = 0;
		static uint32_t s_activeWeapon = 0;
		if (!s_myWeapons) {
			s_myWeapons = Off("CPlayer_WeaponServices->m_hMyWeapons");
			if (!s_myWeapons) s_myWeapons = 0x48; // dump fallback (client_dll.hpp)
		}
		if (!s_activeWeapon) {
			s_activeWeapon = Off("CPlayer_WeaponServices->m_hActiveWeapon");
			if (!s_activeWeapon) s_activeWeapon = 0x60;
		}
		if (!s_myWeapons || !I::GameEntity || !I::GameEntity->Instance)
			return;

		C_CSWeaponBase* active = nullptr;
	{
		if (s_activeWeapon) {
			CBaseHandle ha = SehReadHandle(reinterpret_cast<uint8_t*>(ws) + s_activeWeapon);
			if (ha.valid() && I::GameEntity && I::GameEntity->Instance) {
				C_CSWeaponBase* tmp = I::GameEntity->Instance->Get<C_CSWeaponBase>(ha.index());
				if (tmp && Mem::ValidEntity(tmp)) active = tmp;
			}
		}
	}
	if (!active)
		active = pawn->GetActiveWeapon();

		// Andromeda uses C_NetworkUtlVectorBase: nSize at base, pElements at base+8 (with 4 pad)
		auto* base = reinterpret_cast<uint8_t*>(ws) + s_myWeapons;
		CBaseHandle* elems = nullptr;
		int sz = 0;
		{
			sz = SehReadInt(base + 0);
			elems = SehReadPtr(base + 8);
			if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)) {
				// fallback CUtlVector layout (some builds wrap differently)
				sz = SehReadInt(base + 0x10);
				elems = SehReadPtr(base + 0);
				if (sz <= 0 || sz > 64 || !elems || !Mem::IsUserPtr(elems)) {
					sz = 0; elems = nullptr;
				}
			}
		}

		if (!elems || sz<=0) {
			if (fullRefresh) Con::Ok("SkinChanger: no weapons vector (sz=%d elems=%p off=0x%X)", sz, elems, s_myWeapons);
			return;
		}

		int nWeapons = 0, nApplied = 0, nNoStatic = 0, nNoCfg = 0, nNoView = 0, nOwnerSkip = 0, nNoScene = 0, nNoWeapon = 0;

		for (int i = 0; i < sz; ++i) {
				if (!elems[i].valid())
					continue;
				// Andromeda CHandle::Get = index-only (serial of stored handles
				// can carry identity flags bit; serial-checked Get rejects them).
	auto* w = I::GameEntity->Instance->Get<C_CSWeaponBase>(elems[i].index());
				if (!w || !Mem::ValidEntity(w))
					continue;
				auto* wEnt = reinterpret_cast<C_BaseEntity*>(w);
				++nWeapons;
			// Andromeda gate: only C_BasePlayerWeapon members (designer name
			// "weapon_..."). Rejects stale / non-weapon handles in the vector.
			static uint32_t s_designer = 0;
			if (!s_designer) {
				s_designer = SchemaFinder::Get(hash_32_fnv1a_const("CEntityIdentity->m_designerName"));
				if (!s_designer) s_designer = 0x20; // dump fallback
			}
			void* pIdent = wEnt->m_pEntityIdentity();
			if (!pIdent || !Mem::IsUserPtr(pIdent)) { ++nNoWeapon; continue; }
			// optional designer filter: if offset invalid skip filter (keep weapon)
			if (s_designer) {
				CUtlSymbolLarge sym = *reinterpret_cast<CUtlSymbolLarge*>(reinterpret_cast<uint8_t*>(pIdent) + s_designer);
				const char* dn = sym.String();
				if (!dn || !dn[0] || !strstr(dn, "weapon_")) { ++nNoWeapon; continue; }
			}
			// Owner check: Andromeda is strict (!= steamId). Lefrizzel was permissive but keep strict when steamId known.
			{
				uint64_t owner = OwnerXuid(w);
				if (owner && steamId && owner != steamId) { ++nOwnerSkip; continue; }
				if (!owner && steamId && fullRefresh) {
					// don't skip dropped guns with 0 owner (allow paint); just count
				}
			}
				C_EconItemView* view = ItemView(w);
				if (!view) { ++nNoView; continue; }
				CEconItemDefinition* pDef = SkinSdk::GetStaticData(view);
				if (!pDef) { ++nNoStatic; continue; }
				if (!wEnt->m_pGameSceneNode()) { ++nNoScene; continue; }
				const int curDef = GetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex");
				if (IsGrenade(curDef))
					continue;
				const bool treatAsKnife = pDef->IsKnife(false) || IsDefaultKnife(curDef);
				SkinCfg cfg = Resolve(static_cast<uint16_t>(curDef), treatAsKnife);
				const bool wasSkinned = g_appliedSig.find(elems[i].raw()) != g_appliedSig.end();
				if (!cfg.enabled) {
					if (wasSkinned) {
						// Revert previously skinned weapon/knife to default (config load with no skins / disabled)
						void* comp = SkinSdk::CompositeOwner(w);
						if (treatAsKnife) {
							int team = pawn->m_iTeamNum();
							uint16_t defaultDef = (team == 3) ? 42 : 59;
							CEconItemDefinition* defDefault = SkinSdk::FindDefByIndex(defaultDef);
							const char* defaultModel = defDefault ? defDefault->ModelName() : nullptr;
							if (!defaultModel || !defaultModel[0]) defaultModel = (team == 3) ? "weapons/models/knife/knife_default_ct.vmdl" : "weapons/models/knife/knife_default_t.vmdl";
							SetViewU16(view, "C_EconItemView->m_iItemDefinitionIndex", defaultDef);
							SetViewU32(view, "C_EconItemView->m_iItemIDHigh", 0);
							SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
							SetViewU64(view, "C_EconItemView->m_iItemID", 0);
							SetViewBool(view, "C_EconItemView->m_bInitialized", true);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", 0);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", 0);
							SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", 0.f);
							if (!Off("C_EconEntity->m_nFallbackPaintKit")) *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1680) = 0;
							if (defaultModel) {
								if (w == active && vm && vm->m_pGameSceneNode())
									SkinSdk::SetSceneNodeModel(vm->m_pGameSceneNode(), defaultModel);
								if (wEnt && wEnt->m_pGameSceneNode())
									SkinSdk::SetSceneNodeModel(wEnt->m_pGameSceneNode(), defaultModel);
							}
							uint32_t subOff = Off("C_BaseEntity->m_nSubclassID");
							if (subOff) {
								CUtlStringToken tok(std::to_string(defaultDef).c_str());
								Field<uint32_t>(w, subOff) = tok.m_nHashCode;
							} else {
								CUtlStringToken tok(std::to_string(defaultDef).c_str());
								*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(w) + 0x380) = tok.m_nHashCode;
							}
							SkinSdk::UpdateSubclass(w);
							SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), 2);
							if (w == active && vm && vm->m_pGameSceneNode()) SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), 2);
							if (comp) SkinSdk::UpdateCompositeMaterial(comp);
							SkinSdk::UpdateCompositeMaterialSet(w);
							SkinSdk::UpdateSkin(w);
							SkinSdk::PostDataUpdate(wEnt->m_pGameSceneNode());
							g_appliedSig.erase(elems[i].raw());
							++nApplied;
						} else {
							SetWeaponI32(w, "C_EconEntity->m_nFallbackPaintKit", 0);
							SetWeaponI32(w, "C_EconEntity->m_nFallbackSeed", 0);
							SetWeaponF32(w, "C_EconEntity->m_flFallbackWear", 0.f);
							if (!Off("C_EconEntity->m_nFallbackPaintKit")) *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(w) + 0x1680) = 0;
							SetViewU32(view, "C_EconItemView->m_iItemIDHigh", 0);
							SetViewU32(view, "C_EconItemView->m_iItemIDLow", 0);
							SetViewU64(view, "C_EconItemView->m_iItemID", 0);
							SetViewBool(view, "C_EconItemView->m_bInitialized", true);
							SkinSdk::SetAttributeValueByName(view, "set item texture preference", 0.f);
							SkinSdk::SetAttributeValueByName(view, "set item texture wear", 0.f);
							SkinSdk::SetAttributeValueByName(view, "set item texture seed", 0.f);
							// reset mesh to default (0 = default group)
							SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), 0);
							if (w == active && vm && vm->m_pGameSceneNode()) SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), 0);
							if (comp) SkinSdk::UpdateCompositeMaterial(comp);
							SkinSdk::UpdateCompositeMaterialSet(w);
							SkinSdk::UpdateSkin(w);
							SkinSdk::PostDataUpdate(wEnt->m_pGameSceneNode());
							g_appliedSig.erase(elems[i].raw());
							++nApplied;
						}
					} else {
						++nNoCfg;
					}
					continue;
				}
				CEconItemDefinition* target = (treatAsKnife && cfg.def) ? SkinSdk::FindDefByIndex(cfg.def) : pDef;
				if (!target)
					continue;
				if (!treatAsKnife) {
					if (curDef < 1 || curDef > 70) continue;
					if (IsDefaultKnife(curDef) || (curDef >= 500 && curDef <= 526)) continue;
				}
				cfg.legacy = LookupLegacy(cfg.def ? cfg.def : static_cast<uint16_t>(curDef), cfg.paint);
				cfg.wear = ClampWear(cfg.wear);
				ApplyPaint(w, view, cfg, accountId, treatAsKnife);

				void* composite = SkinSdk::CompositeOwner(w);
				if (!composite) {
					// Andromeda still does composite check, but if null skip heavy but keep paint
					// Keep sig as applied to avoid loop spam; paint already set
					const uint64_t sig2 = MakeSig(cfg);
					g_appliedSig[elems[i].raw()] = sig2;
					++nApplied;
					continue;
				}
				const uint64_t sig = MakeSig(cfg);
				const auto sigIt = g_appliedSig.find(elems[i].raw());
				const bool needHeavy = force || sigIt == g_appliedSig.end() || sigIt->second != sig;
				if (!needHeavy)
					continue;
				const uint64_t meshMask = MeshMask(treatAsKnife, cfg.legacy);
				const bool isActive = (w == active);

				if (treatAsKnife) {
					const char* model = SkinSdk::GetKnifeModelPath(cfg.def ? cfg.def : static_cast<uint16_t>(curDef));
					if (!model) model = target->ModelName();
					if (model && model[0]) {
						if (isActive && vm && vm->m_pGameSceneNode())
							SkinSdk::SetSceneNodeModel(vm->m_pGameSceneNode(), model);
						if (wEnt && wEnt->m_pGameSceneNode())
							SkinSdk::SetSceneNodeModel(wEnt->m_pGameSceneNode(), model);
					}
					const uint32_t subOff = Off("C_BaseEntity->m_nSubclassID");
					if (subOff) {
						CUtlStringToken tok(std::to_string(target->DefIndex()).c_str());
						Field<uint32_t>(w, subOff) = tok.m_nHashCode;
					} else {
						// fallback hard offset 0x380 (dump)
						CUtlStringToken tok(std::to_string(target->DefIndex()).c_str());
						*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(w) + 0x380) = tok.m_nHashCode;
					}
					SkinSdk::UpdateSubclass(w);
				}

				SkinSdk::SetMeshGroupMask(wEnt->m_pGameSceneNode(), meshMask);
				if (isActive && vm && vm->m_pGameSceneNode())
					SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), meshMask);
				SkinSdk::UpdateCompositeMaterial(composite);
				SkinSdk::UpdateCompositeMaterialSet(w);
				SkinSdk::UpdateSkin(w);
				SkinSdk::PostDataUpdate(wEnt->m_pGameSceneNode());
				if (fullRefresh) {
					// Andromeda clears CEconItemView description at 0x200
					Field<uintptr_t>(view, 0x200) = 0;
				}
				g_appliedSig[elems[i].raw()] = sig;
				++nApplied;
			}
		if (fullRefresh)
			Con::Ok("SkinChanger: weapons=%d applied=%d ownerSkip=%d noWeapon=%d noView=%d noStatic=%d noScene=%d noCfg=%d",
				nWeapons, nApplied, nOwnerSkip, nNoWeapon, nNoView, nNoStatic, nNoScene, nNoCfg);
	}

	void SetGlove(C_CSPlayerPawn* pawn)
	{
		C_EconItemView* glove = GloveView(pawn);
		if (!glove)
			return;
		static uint8_t uUpdateFrames = 0;
		static uint16_t s_def = 0;
		static int s_paint = -1;
		static int s_seed = -1;
		static float s_wear = -1.f;

		if (!Config::skin_glove || !SkinSdk::IsSkinnableGloveDef(Config::skin_glove_def)) {
			if (s_def != 0) {
				// Revert glove to default (no glove) - Andromeda parity for config disable
				SetViewU16(glove, "C_EconItemView->m_iItemDefinitionIndex", 0);
				SetViewU32(glove, "C_EconItemView->m_iItemIDHigh", 0);
				SetViewU32(glove, "C_EconItemView->m_iItemIDLow", 0);
				SetViewU64(glove, "C_EconItemView->m_iItemID", 0);
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetAttributeValueByName(glove, "set item texture preference", 0.f);
				uUpdateFrames = 5;
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetBodyGroup(pawn);
				SkinSdk::UpdateBodyGroupChoice(pawn);
				const uint32_t re = Off("C_CSPlayerPawn->m_bNeedToReApplyGloves");
				if (re) Field<bool>(pawn, re) = true;
				s_def = 0; s_paint = -1; s_seed=-1; s_wear=-1.f;
				g_lastSpawn = -1.f;
			} else {
				s_def = 0; s_paint = -1;
			}
			// Still need to keep body group updated for a few frames even when disabling
			if (uUpdateFrames > 0) {
				SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
				SkinSdk::SetBodyGroup(pawn);
				SkinSdk::UpdateBodyGroupChoice(pawn);
				const uint32_t re2 = Off("C_CSPlayerPawn->m_bNeedToReApplyGloves");
				if (re2) Field<bool>(pawn, re2) = true;
				--uUpdateFrames;
			}
			return;
		}
		const uint16_t def = static_cast<uint16_t>(Config::skin_glove_def);
		const int paint = Config::skin_glove_paint;
		const int seed = Config::skin_glove_seed;
		const float wear = ClampWear(Config::skin_glove_wear);
		float spawn = 0.f;
		const uint32_t spawnOff = Off("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
		if (spawnOff)
			spawn = Field<float>(pawn, spawnOff);
		const bool spawned = spawn != g_lastSpawn;
		const bool cfgChanged = s_def != def || s_paint != paint || s_seed != seed || s_wear != wear;
		if (spawned || g_applyGloves || cfgChanged) {
			uUpdateFrames = 5;
			SetViewBool(glove, "C_EconItemView->m_bDisallowSOC", true);
			SetViewBool(glove, "C_EconItemView->m_bRestoreCustomMaterialAfterPrecache", true);
			SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
			SetViewU16(glove, "C_EconItemView->m_iItemDefinitionIndex", def);
			const uint32_t accountId = static_cast<uint32_t>(SkinSdk::InventorySteamId());
			SetViewU32(glove, "C_EconItemView->m_iAccountID", accountId);
			const uint64_t fakeId = 0xF000000000000000ull
				| (static_cast<uint64_t>(def) << 32)
				| static_cast<uint32_t>(paint & 0xFFFF);
			SetViewU64(glove, "C_EconItemView->m_iItemID", fakeId);
			SetViewU32(glove, "C_EconItemView->m_iItemIDHigh", static_cast<uint32_t>(fakeId >> 32));
			SetViewU32(glove, "C_EconItemView->m_iItemIDLow", static_cast<uint32_t>(fakeId));
			if (paint > 0) {
				SkinSdk::SetAttributeValueByName(glove, "set item texture preference", static_cast<float>(paint));
				SkinSdk::SetAttributeValueByName(glove, "set item texture prefab", static_cast<float>(paint));
				SkinSdk::SetAttributeValueByName(glove, "set item texture wear", wear);
				SkinSdk::SetAttributeValueByName(glove, "set item texture seed", static_cast<float>(seed));
			}
			s_def = def; s_paint = paint; s_seed = seed; s_wear = wear;
			g_lastSpawn = spawn;
			g_applyGloves = false;
		}
		if (uUpdateFrames > 0) {
			SetViewBool(glove, "C_EconItemView->m_bInitialized", true);
			SkinSdk::SetBodyGroup(pawn);
			SkinSdk::UpdateBodyGroupChoice(pawn);
			const uint32_t re = Off("C_CSPlayerPawn->m_bNeedToReApplyGloves");
			if (re) Field<bool>(pawn, re) = true;
			--uUpdateFrames;
		}
	}

	void SetAgent(C_CSPlayerPawn* pawn)
	{
		if (!Config::skin_agent) {
			if (g_agentHash != 0) {
				// Revert agent: reset hash so next enable reapplies; try to set default model
				// Default agents: 5036 (T) / 5037 (CT) or first found for team
				int team = pawn->m_iTeamNum();
				uint16_t tryDef = (team == 2) ? 5036 : 5037;
				if (CEconItemDefinition* d = SkinSdk::FindDefByIndex(tryDef); d && d->ModelName() && d->ModelName()[0]) {
					SkinSdk::SetModel(pawn, d->ModelName());
				} else {
					// fallback: find any agent for team from items
					for (auto& it : GetSkinItems().Items()) {
						if (it.type == SkinItems::Agent && it.team == (team == 2 ? 2 : 3) && !it.icon.empty()) {
							// icon holds model path for agents
							SkinSdk::SetModel(pawn, it.icon.c_str());
							break;
						}
					}
				}
				g_agentHash = 0;
			}
			return;
		}
		const int team = pawn->m_iTeamNum();
		const int defIdx = (team == 2) ? Config::skin_agent_t : Config::skin_agent_ct;
		if (defIdx <= 0)
			return;
		CEconItemDefinition* pDef = SkinSdk::FindDefByIndex(static_cast<uint16_t>(defIdx));
		if (!pDef)
			return;
		const char* model = pDef->ModelName();
		if (!model || !model[0] || !pawn->m_pGameSceneNode())
			return;
		uint64_t h = hash_32_fnv1a_const(model);
		h ^= static_cast<uint64_t>(team) << 24;
		if (h == g_agentHash)
			return;
		g_agentHash = h;
		SkinSdk::SetModel(pawn, model);
	}

	bool EventWeaponIsKnife(const char* name)
	{
		if (!name || !name[0]) return false;
		const char* n = name;
		if (!strncmp(n, "weapon_", 7)) n += 7;
		if (!_stricmp(n, "knife") || !_stricmp(n, "knife_t")
			|| !_stricmp(n, "knife_default_ct") || !_stricmp(n, "knife_default_t")
			|| !_stricmp(n, "bayonet"))
			return true;
		return !strncmp(n, "knife_", 6);
	}
}

void SkinChanger::Init()
{
	SkinSdk::Init();
	// Andromeda parity: pre-scan models early (instant, no FileExists) so weapon cfg resolve works even before menu open
	if (!GetSkinItems().Ready())
		GetSkinItems().Scan();
}

void SkinChanger::RefreshAll()
{
	g_appliedSig.clear();
	g_forceReapply = true;
	g_applyGloves = true;
	g_agentHash = 0;
	s_pendingHudClear = 5; // Andromeda HUD retry: clear icon slots for a few frames after skin change / map join
}

void SkinChanger::OnFrameStageNotify(int stage)
{
	if (stage != FRAME_RENDER_START)
		return;
	if (!I::EngineClient || !I::EngineClient->in_game())
		return;
	// Andromeda parity: local inventory is the first gate, before pawn/vm.
	// If inventory null, still try once (first frame after inject inventory may be lazy). Don't block gloves/weapons forever.
	void* inv = SkinSdk::LocalInventory();
	if (!inv) {
		if (g_forceReapply)
			Con::Ok("SkinChanger: LocalInventory null (inventory mgr or +0x3F540) - trying fallback");
		// Fallback: Andromeda's ScanAllItems uses same mgr, but if still null retry next frame
		// Don't return immediately if we can get pawn anyway (weapon paint with accountId 0 still works for gloves)
		// For weapon/knife we need inventory for econ def lookup, but that uses EconSchema not inventory, so allow walk with 0 steamId
	}
	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (!pawn)
		return;
	CCSPlayer_WeaponServices* ws = pawn->GetWeaponServices();
	if (!ws || !Mem::IsUserPtr(ws))
		return;
	C_BaseEntity* vm = SkinSdk::GetViewModel(pawn);
	// A viewmodel is only needed for first-person knife/arms refresh.
	// World weapon entities can still be painted while the HUD viewmodel is
	// delayed during spawn or weapon deploy.
	uint64_t steamId = SkinSdk::InventorySteamId();
	uint32_t accountId = static_cast<uint32_t>(steamId);
	if (!accountId) accountId = 1; // Andromeda fallback: ensure non-zero for SOC (0 would be rejected)
	if (!steamId) steamId = accountId; // keep owner check permissive
	const bool vmChanged = (vm != g_lastVm);
	g_lastVm = vm;
	// HUD refresh triggers: respawn / map-join spawn (m_flLastSpawnTimeIndex) and menu
	// skin selection via RefreshAll. The hud-arms viewmodel entity is recreated on every
	// weapon deploy (vm != g_lastVm) — that churn must never touch HUD icon slots.
	bool spawnChanged = false;
	CBaseHandle curActive{};
	{
		const uint32_t spawnOff = Off("C_CSPlayerPawnBase->m_flLastSpawnTimeIndex");
		float curSpawn = 0.f;
		if (spawnOff) {
			curSpawn = SehReadFloat(reinterpret_cast<uint8_t*>(pawn) + spawnOff);
		}
		static float s_lastHudSpawn = -1.f;
		spawnChanged = (curSpawn != s_lastHudSpawn && curSpawn >= 0.f);
		if (spawnChanged) s_lastHudSpawn = curSpawn;
		const uint32_t offActive = Off("CPlayer_WeaponServices->m_hActiveWeapon");
		const uint32_t useOff = offActive ? offActive : 0x60u;
		curActive = SehReadHandle(reinterpret_cast<uint8_t*>(ws) + useOff);
		if (spawnChanged) {
			if (s_pendingHudClear < 5) s_pendingHudClear = 5;
			g_appliedSig.clear();
			g_forceReapply = true;
		}
	}
	const bool fullRefresh = g_forceReapply;
	const bool force = g_forceReapply;
	g_forceReapply = false;
	// Deploy swap: restore custom knife model on recreated viewmodel (also on spawn — old !spawnChanged guard left knife bugged 3s)
	if (vmChanged && Config::skin_knife && Config::skin_knife_def > 0) {
		const int activeDef = GetWeaponDefFromHandleSafe(curActive);
		if (activeDef == 42 || activeDef == 59 || (activeDef >= 500 && activeDef <= 526)) {
			const char* km = SkinSdk::GetKnifeModelPath(static_cast<uint16_t>(Config::skin_knife_def));
			if (!km) {
				if (CEconItemDefinition* kd = SkinSdk::FindDefByIndex(static_cast<uint16_t>(Config::skin_knife_def)))
					km = kd->ModelName();
			}
			if (km && km[0] && vm && vm->m_pGameSceneNode()) {
				SkinSdk::SetSceneNodeModel(vm->m_pGameSceneNode(), km);
			}
			SkinCfg kcfg{};
			kcfg.enabled = true;
			kcfg.def = static_cast<uint16_t>(Config::skin_knife_def);
			kcfg.paint = Config::skin_knife_paint;
			kcfg.legacy = LookupLegacy(kcfg.def, kcfg.paint);
			if (vm && vm->m_pGameSceneNode())
				SkinSdk::SetMeshGroupMask(vm->m_pGameSceneNode(), MeshMask(true, kcfg.legacy));
		}
	}
	// Don't clear g_appliedSig here - WalkWeapons needs old entries to revert disabled skins
	// It will update/erase per weapon.

	WalkWeapons(pawn, ws, vm, accountId, steamId, force, fullRefresh);
	if (fullRefresh)
		Con::Ok("SkinChanger: apply frame steam=%I64u vm=%p inv=%p", steamId, (void*)vm, (void*)SkinSdk::LocalInventory());
	// HUD icon update: retry for a few frames after skin change / map join / respawn (HUD may not exist yet)
	if (fullRefresh || s_pendingHudClear > 0) {
		if (Config::skin_knife || !Config::skin_weapons.empty()) {
			if (ClearHudIconSlots()) {
				s_pendingHudClear = 0;
			} else if (s_pendingHudClear > 0) {
				--s_pendingHudClear;
			}
		} else {
			s_pendingHudClear = 0;
		}
	}
	SetGlove(pawn);
	SetAgent(pawn);
}

void SkinChanger::OnFireEventClientSide(void* gameEvent)
{
	if (!gameEvent || !Config::skin_knife || Config::skin_knife_def <= 0)
		return;
	auto* ev = reinterpret_cast<IGameEvent*>(gameEvent);
	const char* name = ev->GetName();
	if (!name || _stricmp(name, "player_death"))
		return;
	CCSPlayerController* local = nullptr;
	C_CSPlayerPawn* pawn = H::SafeLocalAlive();
	if (pawn && I::GameEntity && I::GameEntity->Instance) {
		CBaseHandle h = pawn->m_hController();
		if (h.valid())
			local = I::GameEntity->Instance->Get<CCSPlayerController>(h);
	}
	if (!local)
		return;
	CCSPlayerController* attacker = ev->GetPlayerController("attacker");
	if (!attacker || attacker != local)
		return;
	const char* weapon = ev->GetString("weapon");
	if (!EventWeaponIsKnife(weapon))
		return;
	const char* icon = SkinSdk::KnifeIconName(Config::skin_knife_def);
	if (!icon || !icon[0])
		icon = SkinSdk::KnifeWeaponName(Config::skin_knife_def);
	if (icon && icon[0])
		ev->SetString("weapon", icon);
}

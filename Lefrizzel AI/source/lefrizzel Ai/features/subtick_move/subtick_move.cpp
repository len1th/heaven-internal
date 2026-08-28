#define NOMINMAX
#include "subtick_move.h"

#include "../input_inject/input_inject.h"
#include "../aim/aim_common.h"
#include "../prediction/prediction.h"
#include "../movement/jumpbug.h"
#include "../trace/trace.h"
#include "../../config/config.h"
#include "../../utils/cvar/cvar.h"
#include "../../interfaces/CUserCmd/CUserCmd.h"
#include "../../interfaces/CCSGOInput/CCSGOInput.h"
#include "../../interfaces/interfaces.h"
#include "../../utils/console/console.h"
#include "../../utils/schema/schema.h"
#include "../../utils/fnv1a/fnv1a.h"
#include "../../utils/memory/patternscan/patternscan.h"
#include "../../../cs2/entity/C_CSPlayerPawn/C_CSPlayerPawn.h"
#include "../../../cs2/entity/C_BaseEntity/C_BaseEntity.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../hooks/hooks.h"

namespace SubtickMove {
namespace {

constexpr std::uint64_t kJumpMask = IN_JUMP;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.f;
constexpr float kRad2Deg = 180.f / kPi;

bool g_inited = false;

float NormalizeYaw(float yaw)
{
	while (yaw > 180.f) yaw -= 360.f;
	while (yaw < -180.f) yaw += 360.f;
	return yaw;
}

// m_nActualMoveType — ladder / noclip only
bool BadMoveType(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return false;
	__try {
		uint32_t mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_nActualMoveType"));
		if (!mtOff)
			mtOff = SchemaFinder::Get(hash_32_fnv1a_const("C_BaseEntity->m_MoveType"));
		if (!mtOff)
			mtOff = 0x526;
		const uint8_t moveType = *reinterpret_cast<uint8_t*>(
			reinterpret_cast<uintptr_t>(pawn) + mtOff);
		return moveType == MOVETYPE_LADDER || moveType == MOVETYPE_NOCLIP;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

} // namespace

bool Init()
{
	if (g_inited)
		return true;
	g_inited = true;
	InputInject::Init();
	Pred::Init();
	// Warm the bhop cvars at boot — first bhop tick must not do lazy convar
	// resolution on the game thread (the "hitch when enabling bhop").
	Cvar::Float("sv_gravity", 800.f);
	Cvar::Float("sv_standable_normal", 0.7f);
	Cvar::Float("sv_autobunnyhopping", 0.f);
	Con::Ok("SubtickMove ready (bhop: space+ground every tick edge, pre pack+moveSvc)");
	return true;
}

// ============================================================================
// Bhop
// ============================================================================
// Airborne + space held: strip cmd IN_JUMP, predict the landing fraction with a
// hull trace (half-gravity arc, duck-hull adjust, sv_standable_normal gate),
// then emit release@when-1/64 + press@when subtick steps so the server fires
// the hop the instant we touch ground — zero friction dwell.
// Grounded frames: untouched (engine jumps naturally). No CSGOInput pack /
// moveSvc writes — ClearAllSubticks already wiped the
// engine's held-space steps, so the subtick press is a fresh rising edge.

constexpr std::uintptr_t kMoveSvcOff = 0x1248;    // pawn -> m_pMovementServices
constexpr std::uintptr_t kDuckAmountOff = 0x40C;  // CCSPlayer_MovementServices::m_flDuckAmount
constexpr std::uintptr_t kGravityScaleOff = 0x540; // C_BaseEntity::m_flGravityScale

float ReadFloatSeh(void* base, std::uintptr_t off, float fallback)
{
	if (!base)
		return fallback;
	__try {
		const float v = *reinterpret_cast<float*>(
			reinterpret_cast<std::uintptr_t>(base) + off);
		return std::isfinite(v) ? v : fallback;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return fallback;
	}
}

void* GetMoveSvc(C_CSPlayerPawn* pawn)
{
	if (!pawn)
		return nullptr;
	void* svc = nullptr;
	__try {
		svc = pawn->m_pMovementServices();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		svc = nullptr;
	}
	if (svc)
		return svc;
	__try {
		svc = *reinterpret_cast<void**>(
			reinterpret_cast<std::uintptr_t>(pawn) + kMoveSvcOff);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
	return svc;
}

// ============================================================================
// Engine movement hull trace for the landing fraction (trace_player_bbox).
//
// Resolves `patterns::trace_hull` (E8 ?? .. 0F 2F 75 4C) and calls it on
// movement_services + 0x638 with a CTraceFilterPlayerMovementCS filter — 1:1.
// The landing fraction now comes from the SAME hull trace ProcessMovement uses,
// so it matches the server's authoritative landing sub-tick. The generic world
// TraceShape trace disagreed by a quantized 1/64 step on land -> hop fired a
// sub-frame early/late -> extra grounded tick -> the "lag + desync on landing"
// seen online (offline prediction hides it).
// ============================================================================
constexpr std::uintptr_t kMoveTraceCtxOff = 0x638;      // moveSvc -> trace ctx (velocity 1592)

struct EngineTraceResolvers {
	void* moveHull = nullptr;      // sub_18089F9E0
	void* setCollision = nullptr;  // sub_18087CFA0
	bool ready = false;
};

const EngineTraceResolvers& EngineTrace()
{
	static EngineTraceResolvers r;
	if (r.ready)
		return r;
	// sub_18089F9E0 — movement bbox hull trace (single/multi trace dispatch)
	r.moveHull = M::FindPattern("client.dll",
		"48 89 74 24 ? 55 57 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? 48 81 EC A0 01 00 00");
	// sub_18087CFA0 — CTraceFilterPlayerMovementCS (filter, entity, mask, group)
	r.setCollision = M::FindPattern("client.dll",
		"48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 0F B6 41 ? 33 FF C7 41");
	r.ready = r.moveHull && r.setCollision;
	if (!r.ready)
		Con::OffsetMiss("bhop engine movement trace");
	return r;
}

// Engine landing sweep. Returns true with frac/normalZ when the movement
// hull-trace gives a clean floor hit this tick. No sanity-gate
// ladder, no trust latch — call the engine trace directly (it is the same
// ctx ProcessMovement uses; single-shot state is idle between cmds). Any
// fault permanently disables the engine path for the session (falls back to
// the generic world trace).
bool EngineLandingFrac(
	void* moveSvc,
	C_CSPlayerPawn* pawn,
	std::uint64_t mask,
	const Vector_t& start,
	const Vector_t& end,
	const Vector_t& mins,
	const Vector_t& maxs,
	float* outFrac,
	float* outNormalZ)
{
	*outFrac = 0.f;
	*outNormalZ = 0.f;
	const EngineTraceResolvers& r = EngineTrace();
	if (!r.ready || !moveSvc || !pawn)
		return false;

	// Permanent fault latch — one bad call kills the engine path for good
	// (fallback to the generic world trace, which is what ran before this).
	static bool s_faulted = false;
	if (s_faulted)
		return false;

	const auto ctx = reinterpret_cast<std::uint8_t*>(moveSvc) + kMoveTraceCtxOff;
	if (!Mem::IsReadable(ctx, 0x650))
		return false;

	__try {
		alignas(16) std::uint8_t filter[0x80]{};
		reinterpret_cast<void(__fastcall*)(void*, void*, std::uint64_t, int)>(
			r.setCollision)(filter, pawn, mask, 11); // COL_GROUP_PLAYER_MOVEMENT
		float bbox[6] = { mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z };
		Trace::CGameTrace tr{};
		reinterpret_cast<void(__fastcall*)(void*, Trace::CGameTrace*,
			const Vector_t*, const Vector_t*, const float*, const void*)>(
			r.moveHull)(ctx, &tr, &start, &end, bbox, filter);
		const float frac = tr.fraction();
		const float nz = tr.normal().z;
		if (!std::isfinite(frac) || !std::isfinite(nz))
			return false;
		*outFrac = frac;
		*outNormalZ = nz;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		s_faulted = true;
		Con::SehOnce("bhop.engineTrace", GetExceptionCode());
		return false;
	}
}

// predict_landing_fraction — math (prestate = live pre-sim values;
// RewriteBhop runs before Pred::Start, same timing as capture_prestate).
bool PredictLandingFracVelocity(
	C_CSPlayerPawn* pawn,
	void* moveSvc,
	bool holdingDuck,
	float& outFrac)
{
	outFrac = 0.f;
	if (!pawn || !moveSvc)
		return false;

	// prestate.networked_velocity = m_vecVelocity
	Vector_t vel{};
	__try { vel = pawn->m_vecVelocity(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	if (!std::isfinite(vel.z) || vel.z > 0.f)
		return false;

	Vector_t mins{ -16.f, -16.f, 0.f };
	Vector_t maxs{ 16.f, 16.f, 72.f };
	__try {
		if (CCollisionProperty* col = pawn->m_pCollision()) {
			const Vector_t cm = col->m_vecMins();
			const Vector_t cM = col->m_vecMaxs();
			if (std::isfinite(cm.x) && std::isfinite(cM.z) && cM.z > cm.z) {
				mins = cm;
				maxs = cM;
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}

	// prestate.networked_origin = abs origin
	Vector_t traceOrigin{};
	__try {
		CGameSceneNode* node = pawn->m_pGameSceneNode();
		if (node)
			traceOrigin = node->m_vecAbsOrigin();
		else
			traceOrigin = pawn->m_vOldOrigin();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (!std::isfinite(traceOrigin.x) || !std::isfinite(traceOrigin.y)
		|| !std::isfinite(traceOrigin.z))
		return false;

	// Duck hull: drop trace origin toward standing hull, extend maxs up.
	const float duckAmount = ReadFloatSeh(moveSvc, kDuckAmountOff, 0.f);
	if (holdingDuck && duckAmount > 0.f) {
		constexpr float kStandingHeight = 72.f;
		const float duckHullDiff = kStandingHeight - maxs.z;
		traceOrigin.z -= duckHullDiff * 0.5f;
		maxs.z = kStandingHeight;
	}

	// trace_mask read LIVE from the pawn (+0xd48), |= 0x20 when the
	// pawn is invalid or the +0x3f8 flag bit is set — same 1:1 behavior. Falls
	// back to the plain playersolid mask when the read is unsafe.
	std::uint64_t traceMask = 0x201400Bull;
	__try {
		void* pawnBack = *reinterpret_cast<void**>(
			reinterpret_cast<std::uintptr_t>(moveSvc) + 56);
		if (pawnBack && Mem::IsReadable(pawnBack, 0xd50)) {
			traceMask = *reinterpret_cast<std::uint64_t*>(
				reinterpret_cast<std::uint8_t*>(pawnBack) + 0xd48);
			if (*reinterpret_cast<std::uint32_t*>(
					reinterpret_cast<std::uint8_t*>(pawnBack) + 0x3f8) & 0x10)
				traceMask |= 0x20ull;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		traceMask = 0x201400Bull;
	}

	if (!Trace::Ready())
		Trace::Init();

	const float svGravity = Cvar::Float("sv_gravity", 800.f);
	const float svStandable = Cvar::Float("sv_standable_normal", 0.7f);
	const float gravityScale = ReadFloatSeh(pawn, kGravityScaleOff, 1.f);
	if (svGravity <= 0.f || svStandable <= 0.f)
		return false;

	Vector_t v = vel;
	v.z -= (gravityScale * svGravity * Pred::kTickInterval) * 0.5f;

	const Vector_t start = traceOrigin;
	Vector_t end{
		traceOrigin.x + v.x * Pred::kTickInterval,
		traceOrigin.y + v.y * Pred::kTickInterval,
		traceOrigin.z + v.z * Pred::kTickInterval - 2.f
	};

	// Engine movement trace first (server-matching landing sub-tick), then the
	// generic world trace only as a fallback (resolve miss / dirty ctx / AV).
	float frac = 0.f;
	float nz = 0.f;
	bool traced = EngineLandingFrac(moveSvc, pawn, traceMask,
		start, end, mins, maxs, &frac, &nz);
	if (!traced && Trace::Ready()) {
		Trace::CGameTrace tr{};
		if (!Trace::TraceHull(start, end, mins, maxs, pawn, tr, traceMask))
			return false;
		frac = tr.fraction();
		nz = tr.normal().z;
		traced = std::isfinite(frac) && std::isfinite(nz);
	}
	if (!traced)
		return false;
	if (frac <= 0.f || frac >= 1.f)
		return false;
	if (nz < svStandable)
		return false;

	constexpr float kInv64 = 1.f / 64.f;
	outFrac = std::clamp(std::round(frac * 64.f) * kInv64, kInv64, 63.f * kInv64);
	return true;
}

// apply_landing_jump — release just before, press exactly at `when`.
void ApplyLandingJumpVelocity(CBaseUserCmdPB* base, float when)
{
	if (!base)
		return;
	constexpr float kInv64 = 1.f / 64.f;
	const float releaseWhen = std::clamp(when - kInv64, kInv64, 63.f * kInv64);
	if (releaseWhen < when)
		InputInject::SubtickButton(base, IN_JUMP, false, releaseWhen);
	InputInject::SubtickButton(base, IN_JUMP, true, when);
}

// No pre-CreateMove work for bhop — ClearAllSubticks
// in the post-CM pipeline wipes the engine's held-space steps. Kept as a stub
// for the hkCreateMove call site.
void SubtickMove::PreCreateMoveBhop(void* /*pCSGOInput*/)
{
}

void RewriteBhop(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	if (!cmd || !Config::bhop || !pawn)
		return;

	// CONVAR("sv_autobunnyhopping")
	if (Cvar::Float("sv_autobunnyhopping", 0.f) > 0.5f)
		return;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;

	const bool cmdJump = (cmd->nButtons.nValue & kJumpMask) != 0
		|| (cmd->nButtons.nValueChanged & kJumpMask) != 0
		|| (cmd->nButtons.nValueScroll & kJumpMask) != 0
		|| (base && base->pInButtonState
			&& ((base->pInButtonState->nValue & kJumpMask)
				|| (base->pInButtonState->nValueChanged & kJumpMask)
				|| (base->pInButtonState->nValueScroll & kJumpMask)));
	const bool keySpace = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
	if (!cmdJump && !keySpace)
		return;

	if (JumpBug::ClaimedJumpThisTick())
		return;

	if (BadMoveType(pawn))
		return;

	std::uint32_t flags = 0;
	__try { flags = pawn->m_fFlags(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }

	const bool onGround = (flags & FL_ONGROUND) != 0;

	if (!onGround) {
		// Strip jump bits by default while airborne
		cmd->nButtons.nValue &= ~kJumpMask;
		cmd->nButtons.nValueChanged &= ~kJumpMask;
		cmd->nButtons.nValueScroll &= ~kJumpMask;
		if (base && base->pInButtonState) {
			base->pInButtonState->nValue &= ~kJumpMask;
			base->pInButtonState->nValueChanged &= ~kJumpMask;
			base->pInButtonState->nValueScroll &= ~kJumpMask;
			base->pInButtonState->SetBits(
				BUTTON_STATE_PB_BITS_BUTTONSTATE1
				| BUTTON_STATE_PB_BITS_BUTTONSTATE2
				| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
			base->SetBits(BASE_BITS_BUTTONPB);
		}

		void* moveSvc = GetMoveSvc(pawn);
		if (moveSvc && base) {
			const bool holdingDuck = (cmd->nButtons.nValue & IN_DUCK) != 0
				|| (cmd->nButtons.nValueChanged & IN_DUCK) != 0;
			float landFrac = 0.f;
			if (PredictLandingFracVelocity(pawn, moveSvc, holdingDuck, landFrac)) {
				// We land this tick: re-inject jump into cmd bitmask and subtick queue
				cmd->nButtons.nValue |= kJumpMask;
				cmd->nButtons.nValueChanged |= kJumpMask;
				cmd->nButtons.nValueScroll |= kJumpMask;
				if (base->pInButtonState) {
					base->pInButtonState->nValue |= kJumpMask;
					base->pInButtonState->nValueChanged |= kJumpMask;
					base->pInButtonState->nValueScroll |= kJumpMask;
					base->pInButtonState->SetBits(
						BUTTON_STATE_PB_BITS_BUTTONSTATE1
						| BUTTON_STATE_PB_BITS_BUTTONSTATE2
						| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
					base->SetBits(BASE_BITS_BUTTONPB);
				}
				ApplyLandingJumpVelocity(base, landFrac);
			}
		}
	} else {
		// On ground: ensure fresh jump rising edge with scroll value and subtick press
		cmd->nButtons.nValue |= kJumpMask;
		cmd->nButtons.nValueChanged |= kJumpMask;
		cmd->nButtons.nValueScroll |= kJumpMask;
		if (base) {
			if (base->pInButtonState) {
				base->pInButtonState->nValue |= kJumpMask;
				base->pInButtonState->nValueChanged |= kJumpMask;
				base->pInButtonState->nValueScroll |= kJumpMask;
				base->pInButtonState->SetBits(
					BUTTON_STATE_PB_BITS_BUTTONSTATE1
					| BUTTON_STATE_PB_BITS_BUTTONSTATE2
					| BUTTON_STATE_PB_BITS_BUTTONSTATE3);
				base->SetBits(BASE_BITS_BUTTONPB);
			}
			InputInject::SubtickButton(base, IN_JUMP, true, 0.0f);
		}
	}
}

// ============================================================================
// Autostrafe (mode 1) — WASD subtick
// ============================================================================
// Quantized-movement air strafer: while airborne with WASD edge-latched it
// walks a 16-subframe AirAccelerate simulation, emitting yaw_delta subtick
// steps so the server turns into the wish direction every subframe
// (sv_quantize_movement_input must be on). Base moves are left untouched;
// hooks.cpp zeroes them when no yaw steps were injected (final
// subtick). Rage-fire gate has no equivalent here — no rage module.

constexpr int kStrafeMaxSubticks = 16;
constexpr float kStrafeMinSpeed = 5.0f;
constexpr std::uintptr_t kSurfaceFrictionOff = 0x26C; // CPlayer_MovementServices_Humanoid

bool g_strafeHandledThisTick = false;
std::uint64_t g_strafeLastButtons = 0;
std::uint64_t g_strafeLastPressed = 0;
int g_strafeSubstepCounter = 0;

float GetMaxSubtickWhen(CBaseUserCmdPB* base)
{
	float maxWhen = 0.f;
	if (!base || !base->subtickMovesField.pRep)
		return maxWhen;
	const int n = base->subtickMovesField.nCurrentSize;
	for (int i = 0; i < n; ++i) {
		CSubtickMoveStep* step = base->subtickMovesField.pRep->tElements[i];
		if (step)
			maxWhen = (std::max)(maxWhen, step->flWhen);
	}
	return maxWhen;
}

// ref_ideal_angle — optimal wish offset from the velocity vector.
float RefIdealAngle(float speed, float dt, float wishspeed, float airAccel, float airMaxWishspeed)
{
	if (speed < 1.0f)
		return 15.0f;
	const float accelSpeed = wishspeed * airAccel * dt;
	float cosTheta = 0.f;
	if (accelSpeed >= airMaxWishspeed)
		cosTheta = airMaxWishspeed / (2.0f * speed);
	else
		cosTheta = (airMaxWishspeed - accelSpeed) / speed;
	cosTheta = std::clamp(cosTheta, -1.f, 1.f);
	return (std::max)(std::acos(cosTheta) * kRad2Deg, 1.0f);
}

// ref_air_strafer — wish yaw that gains speed toward target_yaw.
float RefAirStrafer(float velX, float velY, float targetYaw, float dt, bool sideSwitch,
	float wishspeed, float airAccel, float airMaxWishspeed)
{
	const float speed = std::sqrt(velX * velX + velY * velY);
	const float theta = RefIdealAngle(speed, dt, wishspeed, airAccel, airMaxWishspeed);
	if (speed < 15.0f)
		return targetYaw;
	const float velAngle = std::atan2(velY, velX) * kRad2Deg;
	const float velDelta = NormalizeYaw(targetYaw - velAngle);
	if (std::fabs(velDelta) > 2.0f) {
		if (velDelta > 0.0f)
			return NormalizeYaw(velAngle + theta);
		return NormalizeYaw(velAngle - theta);
	}
	if (sideSwitch)
		return NormalizeYaw(velAngle + theta);
	return NormalizeYaw(velAngle - theta);
}

// ref_air_accel_sim — one AirAccelerate substep on the sim velocity.
void RefAirAccelSim(float& velX, float& velY, float wishdirYaw, float frameTime,
	float friction, float wishspeed, float airAccel, float airMaxWishspeed)
{
	const float yawRad = wishdirYaw * kDeg2Rad;
	const float wishDirX = std::cos(yawRad);
	const float wishDirY = std::sin(yawRad);
	const float capped = (std::min)(wishspeed, airMaxWishspeed);
	const float dot = velX * wishDirX + velY * wishDirY;
	const float addSpeed = capped - dot;
	if (addSpeed <= 0.f)
		return;
	const float accelSpeed = wishspeed * airAccel * friction * frameTime;
	const float step = (std::min)(accelSpeed, addSpeed);
	velX += wishDirX * step;
	velY += wishDirY * step;
}

// movement_from_buttons — verbatim signs (A → left -1, D → left +1).
void StrafeMovementFromButtons(std::uint64_t pressed, float& outFwd, float& outLeft)
{
	outFwd = 0.f;
	outLeft = 0.f;
	if (pressed & IN_FORWARD) outFwd = 1.f;
	else if (pressed & IN_BACK) outFwd = -1.f;
	if (pressed & IN_MOVELEFT) outLeft = -1.f;
	else if (pressed & IN_MOVERIGHT) outLeft = 1.f;
}

// check_button — edge-latch a pressed key, releasing its opposite.
void StrafeCheckButton(std::uint64_t currentButtons, std::uint64_t button)
{
	const bool moveLeft = (button & IN_MOVELEFT) != 0;
	const bool moveRight = (button & IN_MOVERIGHT) != 0;
	const bool fwd = (button & IN_FORWARD) != 0;
	const bool back = (button & IN_BACK) != 0;

	if ((currentButtons & button) != 0
		&& ((g_strafeLastButtons & button) == 0
			|| (moveLeft && (g_strafeLastPressed & IN_MOVERIGHT) == 0)
			|| (moveRight && (g_strafeLastPressed & IN_MOVELEFT) == 0)
			|| (fwd && (g_strafeLastPressed & IN_BACK) == 0)
			|| (back && (g_strafeLastPressed & IN_FORWARD) == 0))) {
		if (moveLeft) g_strafeLastPressed &= ~IN_MOVERIGHT;
		else if (moveRight) g_strafeLastPressed &= ~IN_MOVELEFT;
		else if (fwd) g_strafeLastPressed &= ~IN_BACK;
		else if (back) g_strafeLastPressed &= ~IN_FORWARD;
		g_strafeLastPressed |= button;
	} else if ((currentButtons & button) == 0) {
		g_strafeLastPressed &= ~button;
	}
}

// quantized_path — 16-subframe yaw-delta injection.
void QuantizedPath(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	const std::uint64_t currentButtons = cmd->nButtons.nValue;
	if (currentButtons & IN_SPRINT)
		return;

	CBaseUserCmdPB* base = cmd->csgoUserCmd.pBaseCmd;
	if (!base)
		return;

	StrafeCheckButton(currentButtons, IN_MOVELEFT);
	StrafeCheckButton(currentButtons, IN_MOVERIGHT);
	StrafeCheckButton(currentButtons, IN_FORWARD);
	StrafeCheckButton(currentButtons, IN_BACK);
	g_strafeLastButtons = currentButtons;

	// prestate = live pre-sim values (RewriteStrafe runs before Pred::Start).
	// Use m_vecVelocity (networked); fall back to abs velocity when
	// the networked field is stale/zero on this build.
	Vector_t velocity{};
	__try {
		velocity = pawn->m_vecVelocity();
		if (std::fabs(velocity.x) < 0.01f && std::fabs(velocity.y) < 0.01f
			&& std::fabs(velocity.z) < 0.01f)
			velocity = pawn->m_vecAbsVelocity();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return;
	}
	if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
		return;
	const float speed2d = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);

	float commandYaw = 0.f;
	if (base->pViewAngles) {
		commandYaw = base->pViewAngles->angValue.y;
	} else {
		// PB view missing → never steer relative to 0° (strafer would fight the
		// player and gain nothing). Fall back to the live input view.
		QAngle_t v{};
		__try {
			if (AimCommon::GetViewAngles(v) && v.IsValid())
				commandYaw = v.y;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	float fwd = 0.f;
	float left = 0.f;
	StrafeMovementFromButtons(g_strafeLastPressed, fwd, left);
	if (fwd == 0.f && left == 0.f)
		return;
	if (speed2d < kStrafeMinSpeed)
		return;

	const float startWhen = GetMaxSubtickWhen(base);
	if (startWhen >= 0.99f)
		return;

	const float svAirAccelerate = Cvar::Float("sv_airaccelerate", 12.f);
	const float svMaxSpeed = Cvar::Float("sv_maxspeed", 250.f);
	const float svAirMaxWishspeed = Cvar::Float("sv_air_max_wishspeed", 30.f);
	const float surfaceFriction = ReadFloatSeh(GetMoveSvc(pawn), kSurfaceFrictionOff, 1.f);

	const float baseYawOffset = std::atan2(-left, fwd) * kRad2Deg;
	const float targetYaw = NormalizeYaw(commandYaw + baseYawOffset);

	const float subFrame = Pred::kTickInterval / static_cast<float>(kStrafeMaxSubticks);
	const float whenStep = (1.0f - startWhen) / static_cast<float>(kStrafeMaxSubticks + 1);

	float accYaw = commandYaw;
	float simVx = velocity.x;
	float simVy = velocity.y;
	int injected = 0;

	for (int i = 1; i <= kStrafeMaxSubticks; ++i) {
		const bool entrySide = ((g_strafeSubstepCounter + i) % 2) == 0;
		const float wishdirYaw = RefAirStrafer(simVx, simVy, targetYaw, subFrame,
			entrySide, svMaxSpeed, svAirAccelerate, svAirMaxWishspeed);
		const float targetViewYaw = NormalizeYaw(wishdirYaw - baseYawOffset);
		const float yawDelta = NormalizeYaw(targetViewYaw - accYaw);
		const float whenFrac = startWhen + static_cast<float>(i) * whenStep;
		if (!InputInject::SubtickYawDelta(base, whenFrac, yawDelta))
			break;
		accYaw = targetViewYaw;
		RefAirAccelSim(simVx, simVy, wishdirYaw, subFrame, surfaceFriction,
			svMaxSpeed, svAirAccelerate, svAirMaxWishspeed);
		++injected;
	}

	if (injected > 0) {
		g_strafeHandledThisTick = true;
		++g_strafeSubstepCounter;
	}
}

void RewriteStrafe(CUserCmd* cmd, C_CSPlayerPawn* pawn)
{
	// Reset handled every tick first
	g_strafeHandledThisTick = false;

	if (!cmd || !pawn)
		return;
	// menu: mode 1 = Silent WASD Subtick
	if (!Config::autostrafe || Config::autostrafe_mode != 1)
		return;

	// is_active: sv_quantize_movement_input must be on (MM default 1).
	// Non-blocking: the client-side convar copy can read 0 before replication
	// while the server runs 1 — gating on it killed the strafer. Official
	// servers always run quantized movement; on servers with it off the yaw
	// steps are ignored harmlessly.
	{
		const float quant = Cvar::Float("sv_quantize_movement_input", 1.f);
		if (quant <= 0.5f) {
			static bool s_quantLogged = false;
			if (!s_quantLogged) {
				s_quantLogged = true;
				Con::Warn("WASD subtick strafer: sv_quantize_movement_input reads %.0f "
					"(server may not run quantized movement)", quant);
			}
		}
	}

	if (!cmd->csgoUserCmd.pBaseCmd)
		return;

	if (JumpBug::ClaimedJumpThisTick())
		return;

	// ladder / noclip via m_nActualMoveType
	if (BadMoveType(pawn))
		return;

	// prestate.flags & on_ground
	std::uint32_t flags = 0;
	__try { flags = pawn->m_fFlags(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return; }
	if (flags & FL_ONGROUND)
		return;

	// quantized_path (WASD edge-latch + 16 yaw_delta subticks)
	QuantizedPath(cmd, pawn);
}

bool HandledThisTick()
{
	return g_strafeHandledThisTick;
}

} // namespace SubtickMove

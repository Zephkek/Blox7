#!/usr/bin/env bash
# real startup smoke. A, B and LEGACY are diagnostics, C = luaapp frame, GAME = local ugcgame,
# INPUT = a real click on top, NETWORK = signed-in public join (account bits never printed)

set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKING_DIRECTORY="${BLOX7_WORKING_DIRECTORY:-${ROOT}}"
[[ -d "${WORKING_DIRECTORY}" ]] || {
  echo "missing runtime working directory: ${WORKING_DIRECTORY}" >&2
  exit 2
}
cd "${WORKING_DIRECTORY}"

BIN="${BLOX7_BIN:-${ROOT}/build/blox7}"
TIER="${1:-A}"
if (( $# > 0 )); then
  shift
fi
USER_ARGUMENTS=("$@")

ValidateHostAbiProbationBoundary() {
  local allow_unverified=false argument
  for argument in "${USER_ARGUMENTS[@]}"; do
    [[ "${argument}" != --allow-unverified-build ]] ||
      allow_unverified=true
  done
  if [[ "${BLOX7_HOST_ABI_CANARY:-0}" == 1 ]]; then
    [[ "${BLOX7_ISOLATED_CANARY:-0}" == 1 &&
       "${BLOX7_ALLOW_CANDIDATE_HOST_ABI:-0}" == 1 &&
       "${allow_unverified}" == true ]] || {
      echo "candidate host-ABI profile requires isolated probation" >&2
      return 2
    }
    [[ -z "${BLOX7_HOST_ABI_APPROVAL_RECEIPT:-}" ]] || {
      echo "probation must not reuse an approval receipt" >&2
      return 2
    }
    local probation_artifact
    for probation_artifact in \
        "${BLOX7_HOST_ABI_PROFILE_FILE:-}" \
        "${BLOX7_COMPATIBILITY_MANIFEST:-}"; do
      [[ "${probation_artifact}" == /* &&
         -f "${probation_artifact}" &&
         ! -L "${probation_artifact}" ]] || {
        echo "probation host-ABI artifact is not a regular absolute path" >&2
        return 2
      }
    done
    return 0
  fi
  if [[ "${BLOX7_ALLOW_CANDIDATE_HOST_ABI:-0}" == 1 ]]; then
    echo "candidate host-ABI permission is valid only inside probation" >&2
    return 2
  fi
}

ValidateHostAbiProbationBoundary
LOG_DIR="${BLOX7_LOG_DIR:-${ROOT}/logs/real_bringup}"
mkdir -p "${LOG_DIR}"
STAMP="$(date +%Y%m%d-%H%M%S)"
if [[ "${TIER}" == "NETWORK" ]]; then
  LOG="$(mktemp "${LOG_DIR}/tierNETWORK_${STAMP}_XXXXXX.log")"
else
  LOG="${LOG_DIR}/tier${TIER}_${STAMP}.log"
fi

if [[ ! -x "${BIN}" ]]; then
  echo "missing binary: ${BIN}" >&2
  exit 2
fi

PrepareActiveAssetLink() {
  local data_root current_manifest payload_path assets_root rbx_root
  data_root="${BLOX7_DATA_ROOT:-${XDG_DATA_HOME:-${HOME}/.local/share}/blox7}"
  current_manifest="${data_root}/current.json"
  [[ -f "${current_manifest}" && ! -L "${current_manifest}" ]] || return 0
  command -v jq >/dev/null 2>&1 || return 0
  payload_path="$(jq -er '.payload_path | select(test("^payloads/[A-Za-z0-9._-]+$"))' \
    "${current_manifest}" 2>/dev/null || true)"
  [[ -n "${payload_path}" ]] || return 0
  assets_root="${data_root}/${payload_path}/assets"
  [[ -d "${assets_root}" && ! -L "${assets_root}" ]] || return 0
  rbx_root="${WORKING_DIRECTORY}/rbx_bin"
  if [[ -e "${rbx_root}" && ! -d "${rbx_root}" ]]; then
    echo "invalid runtime asset-link parent: ${rbx_root}" >&2
    return 1
  fi
  mkdir -p -- "${rbx_root}"
  if [[ -d "${rbx_root}/assets" && ! -L "${rbx_root}/assets" ]]; then
    return 0
  fi
  ln -sfn -- "${assets_root}" "${rbx_root}/assets"
}

case "${TIER}" in
  C|GAME|INPUT|RESIZE|NETWORK)
    if [[ "${BLOX7_SKIP_UPDATE_CHECK:-0}" != 1 ]]; then
      "${ROOT}/scripts/auto_update_roblox.sh" --skip-build --no-launch
      export BLOX7_SKIP_UPDATE_CHECK=1
    fi
    PrepareActiveAssetLink
    ;;
esac

export BLOX7_VALIDATE_ROBLOX_COOKIE="${BLOX7_VALIDATE_ROBLOX_COOKIE:-0}"
if [[ "${BLOX7_ISOLATED_CANARY:-0}" == 1 ]]; then
  # canaries never touch the user's cookies, the guest luaapp is enough
  unset BLOX7_COOKIE_FILE BLOX7_ROBLOX_COOKIES
  export BLOX7_ALLOW_NO_COOKIE_LUA_APP=1
fi
export BLOX7_ENGINE_DETACH="${BLOX7_ENGINE_DETACH:-0}"
export BLOX7_CALL_REAL_APP_BRIDGE_INIT="${BLOX7_CALL_REAL_APP_BRIDGE_INIT:-1}"
export BLOX7_CALL_REAL_APP_BRIDGE_INIT_THREAD="${BLOX7_CALL_REAL_APP_BRIDGE_INIT_THREAD:-0}"
export BLOX7_ENGINE_TRACE="${BLOX7_ENGINE_TRACE:-1}"
export BLOX7_SKIP_LIBROBLOX_CTORS="${BLOX7_SKIP_LIBROBLOX_CTORS:-0}"
# the build-id profile picks roblox's own mimalloc, BLOX7_HOST_ALLOCATOR_BRIDGES=1 is for research
export BLOX7_HOST_JNI_SINGLETON_SEED="${BLOX7_HOST_JNI_SINGLETON_SEED:-0}"
export BLOX7_INIT_CLIENT_SETTINGS="${BLOX7_INIT_CLIENT_SETTINGS:-1}"

# 2.725's mt pack loader does fseek+fread on one shared FILE, so nvidia (0x10de = 4318) gets the mt shader deny
DEFAULT_VULKAN_CLIENT_SETTINGS_OVERRIDES='{"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"}'

case "${TIER}" in
  A)
    # Load + JNI_OnLoad + GlobalInit + real V2Init only
    export BLOX7_HEADLESS=1
    export BLOX7_KEEPALIVE_MS="${BLOX7_KEEPALIVE_MS:-8000}"
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-60000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_START=0
    export BLOX7_STEP_START_APP_WITH_PARAMS=0
    export BLOX7_START_LUA_APP_DM=0
    export BLOX7_STEP_START_LUA_APP_DM=0
    export BLOX7_START_GAME_WITH_PARAM=0
    export BLOX7_SEND_APP_READY=0
    export BLOX7_SEND_GAME_LOADED=0
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    EXTRA=(--allow-unverified-build --headless)
    ;;
  B)
    # + real StartApp
    export BLOX7_HEADLESS=1
    export BLOX7_KEEPALIVE_MS="${BLOX7_KEEPALIVE_MS:-15000}"
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-90000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_START=1
    export BLOX7_STEP_START_APP_WITH_PARAMS=1
    export BLOX7_START_LUA_APP_DM=0
    export BLOX7_STEP_START_LUA_APP_DM=0
    export BLOX7_START_GAME_WITH_PARAM=0
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    EXTRA=(--allow-unverified-build --headless)
    ;;
  C)
    # luaapp in a real window on roblox's vulkan backend, no game surface until the first frame
    export BLOX7_HEADLESS=0
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-120000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_START=1
    export BLOX7_STEP_START_APP_WITH_PARAMS=1
    export BLOX7_UPDATE_SURFACE_APP_AFTER_START_APP=1
    export BLOX7_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE=1
    export BLOX7_START_LUA_APP_DM="${BLOX7_START_LUA_APP_DM:-1}"
    export BLOX7_STEP_START_LUA_APP_DM="${BLOX7_STEP_START_LUA_APP_DM:-1}"
    export BLOX7_POST_CLIENT_SETTINGS="${BLOX7_POST_CLIENT_SETTINGS:-1}"
    export BLOX7_STEP_POST_CLIENT_SETTINGS="${BLOX7_STEP_POST_CLIENT_SETTINGS:-1}"
    export BLOX7_FETCH_CLIENT_SETTINGS="${BLOX7_FETCH_CLIENT_SETTINGS:-1}"
    # loadDataModel starts luaapp after V2Init, the surface callback starts AppBridge after that. same order as the apk
    export BLOX7_START_LUA_APP_DM_AFTER_START_APP="${BLOX7_START_LUA_APP_DM_AFTER_START_APP:-0}"
    export BLOX7_START_LUA_APP_DM_INLINE="${BLOX7_START_LUA_APP_DM_INLINE:-1}"
    export BLOX7_START_LUA_APP_DM_DELAY_MS="${BLOX7_START_LUA_APP_DM_DELAY_MS:-0}"
    export BLOX7_START_GAME_WITH_PARAM=0
    export BLOX7_RESUME_GAME_WITH_PLATFORM_PARAMS_AFTER_START_GAME=0
    export BLOX7_SEND_APP_READY=0
    export BLOX7_SEND_GAME_LOADED=0
    export BLOX7_MAIN_THREAD_MESSAGE_PUMP=1
    export BLOX7_GRAPHICS_BACKEND="${BLOX7_GRAPHICS_BACKEND:-direct-vulkan}"
    export BLOX7_PRELOAD_VULKAN_SHIM=1
    # no forced gles ubo translator, roblox picks vulkan once VK_KHR_android_surface is there
    export BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON="${BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON:-${DEFAULT_VULKAN_CLIENT_SETTINGS_OVERRIDES}}"
    export BLOX7_REQUIRE_REAL_GRAPHICS=1
    # the gate reads the present serial from this trace, so no override
    export BLOX7_WINDOW_TRACE=1
    export BLOX7_EGL_TRACE="${BLOX7_EGL_TRACE:-1}"
    # hold the first frame a bit, then tear roblox down before sdl
    export BLOX7_AUTO_EXIT_AFTER_PRESENT_MS="${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS:-3000}"
    # C is the supported path for the exact build id, normal gate
    EXTRA=(--windowed)
    ;;
  GAME|INPUT|RESIZE)
    # ugcgame once luaapp works. the first surface starts the game, Update/Resume are for running/paused ones
    export BLOX7_UPDATE_SURFACE_GAME_BEFORE_START_GAME=0
    export BLOX7_RESUME_GAME_WITH_PLATFORM_PARAMS_AFTER_START_GAME=0
    export BLOX7_HEADLESS=0
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-120000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_START=1
    export BLOX7_START_LUA_APP_DM=1
    export BLOX7_START_GAME_WITH_PARAM=1
    # restore path only, not StartGame
    export BLOX7_SEND_GAME_LOADED=0
    export BLOX7_FETCH_CLIENT_SETTINGS="${BLOX7_FETCH_CLIENT_SETTINGS:-1}"
    export BLOX7_GRAPHICS_BACKEND="${BLOX7_GRAPHICS_BACKEND:-direct-vulkan}"
    export BLOX7_PRELOAD_VULKAN_SHIM=1
    export BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON="${BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON:-${DEFAULT_VULKAN_CLIENT_SETTINGS_OVERRIDES}}"
    export BLOX7_REQUIRE_REAL_GRAPHICS=1
    # the gate counts vkQueuePresentKHR serials from this trace
    export BLOX7_WINDOW_TRACE=1
    export BLOX7_AUTO_EXIT_AFTER_PRESENT_MS="${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS:-4000}"
    if [[ "${TIER}" == "INPUT" ]]; then
      export BLOX7_INPUT_TEST_CLICK=1
    fi
    if [[ "${TIER}" == "RESIZE" ]]; then
      export BLOX7_RESIZE_READINESS=1
      export BLOX7_RESIZE_WIDTH="${BLOX7_RESIZE_WIDTH:-1024}"
      export BLOX7_RESIZE_HEIGHT="${BLOX7_RESIZE_HEIGHT:-640}"
    fi
    # same profile and gate as C
    EXTRA=(--windowed)
    ;;
  NETWORK)
    # signed-in public join: needs a place id and an account, no private/reserved/follow joins
    place_id="${BLOX7_PLACE_ID:-}"
    if [[ ! "${place_id}" =~ ^[1-9][0-9]{0,18}$ ]] ||
       { [[ "${#place_id}" -eq 19 ]] &&
         [[ "${place_id}" > "9223372036854775807" ]]; }; then
      echo "NETWORK requires BLOX7_PLACE_ID to be a positive signed 64-bit integer" >&2
      exit 2
    fi
    if [[ -n "${BLOX7_ROBLOX_COOKIES:-}" ]]; then
      echo "NETWORK requires a private cookie file, not a credential in process environment" >&2
      exit 2
    fi
    if [[ -z "${BLOX7_COOKIE_FILE:-}" ||
          ! -f "${BLOX7_COOKIE_FILE}" ||
          -L "${BLOX7_COOKIE_FILE}" ||
          ! -r "${BLOX7_COOKIE_FILE}" ]]; then
      echo "NETWORK requires a readable private regular cookie file" >&2
      exit 2
    fi
    cookie_mode="$(stat -c '%a' -- "${BLOX7_COOKIE_FILE}")"
    if (( (8#${cookie_mode: -3} & 8#077) != 0 )); then
      echo "NETWORK cookie file must not be accessible by group or other users" >&2
      exit 2
    fi

    if [[ -n "${BLOX7_GAME_JOIN_REQUEST_TYPE:-}" &&
          "${BLOX7_GAME_JOIN_REQUEST_TYPE}" != "0" ]]; then
      echo "NETWORK public-place join requires join request type 0" >&2
      exit 2
    fi
    if [[ -n "${BLOX7_GAME_JOIN_USER_ID:-}" &&
          "${BLOX7_GAME_JOIN_USER_ID}" != "0" ]]; then
      echo "NETWORK public-place join requires an empty join target" >&2
      exit 2
    fi
    public_join_excluded_variables=(
      BLOX7_GAME_ACCESS_CODE
      BLOX7_GAME_LINK_CODE
      BLOX7_GAME_RESERVED_SERVER_ACCESS_CODE
      BLOX7_GAME_CONVERSATION_ID
      BLOX7_REFERRED_BY_PLAYER_ID
      BLOX7_GAME_CALL_ID
      BLOX7_GAME_EVENT_ID
      BLOX7_GAME_JOIN_ATTEMPT_ID
      BLOX7_GAME_JOIN_ATTEMPT_ORIGIN
      BLOX7_GAME_ISO_CONTEXT
      BLOX7_GAME_ID
      BLOX7_ROBLOX_USERNAME
      BLOX7_REFERRAL_PAGE
      BLOX7_GAME_JOIN_CONTEXT
      BLOX7_GAME_PARAMS_JSON
    )
    for variable_name in "${public_join_excluded_variables[@]}"; do
      if [[ -n "${!variable_name:-}" ]]; then
        echo "NETWORK public-place join rejects special join selectors" >&2
        exit 2
      fi
    done

    export BLOX7_ALLOW_NO_COOKIE_LUA_APP=0
    export BLOX7_GAME_JOIN_REQUEST_TYPE=0
    export BLOX7_GAME_JOIN_USER_ID=0
    export BLOX7_UPDATE_SURFACE_GAME_BEFORE_START_GAME=0
    export BLOX7_RESUME_GAME_WITH_PLATFORM_PARAMS_AFTER_START_GAME=0
    export BLOX7_HEADLESS=0
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-120000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_START=1
    export BLOX7_START_LUA_APP_DM=1
    export BLOX7_START_GAME_WITH_PARAM=1
    # onGameLoaded has to come from the joined DataModel, not the restore event
    export BLOX7_SEND_GAME_LOADED=0
    export BLOX7_FETCH_CLIENT_SETTINGS="${BLOX7_FETCH_CLIENT_SETTINGS:-1}"
    export BLOX7_GRAPHICS_BACKEND="${BLOX7_GRAPHICS_BACKEND:-direct-vulkan}"
    export BLOX7_PRELOAD_VULKAN_SHIM=1
    export BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON="${BLOX7_CLIENT_SETTINGS_OVERRIDES_JSON:-${DEFAULT_VULKAN_CLIENT_SETTINGS_OVERRIDES}}"
    export BLOX7_REQUIRE_REAL_GRAPHICS=1
    export BLOX7_WINDOW_TRACE=1
    export BLOX7_AUTO_EXIT_AFTER_PRESENT_MS="${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS:-20000}"
    if [[ ! "${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS}" =~ ^[0-9]{1,9}$ ]] ||
       (( 10#${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS} < 20000 )); then
      echo "NETWORK requires at least 20000 ms after the first present" >&2
      exit 2
    fi
    export BLOX7_SMOKE_TIMEOUT_S="${BLOX7_SMOKE_TIMEOUT_S:-120}"
    if [[ ! "${BLOX7_SMOKE_TIMEOUT_S}" =~ ^[0-9]{1,9}$ ]] ||
       (( 10#${BLOX7_SMOKE_TIMEOUT_S} < 90 )); then
      echo "NETWORK requires a smoke timeout of at least 90 seconds" >&2
      exit 2
    fi
    # NETWORK only runs on a supported build id
    EXTRA=(--windowed)
    ;;
  LEGACY)
    # Researched 2.721.1108 with offset patches enabled by profile
    OLD="${ROOT}/rbx_bin/versions/2.721.1108-50e1b0abd123350e794226062fe3a1ef360c5f0d/libroblox.so"
    export BLOX7_HEADLESS=1
    export BLOX7_KEEPALIVE_MS="${BLOX7_KEEPALIVE_MS:-20000}"
    export BLOX7_STARTUP_THREAD_TIMEOUT_MS="${BLOX7_STARTUP_THREAD_TIMEOUT_MS:-90000}"
    export BLOX7_CALL_REAL_APP_BRIDGE_INIT=1
    export BLOX7_CALL_REAL_APP_BRIDGE_START=1
    export BLOX7_START_LUA_APP_DM=1
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    EXTRA=(--allow-unverified-build --headless --roblox-lib "${OLD}")
    ;;
  *)
    echo "usage: $0 A|B|C|GAME|INPUT|RESIZE|NETWORK|LEGACY" >&2
    exit 2
    ;;
esac

echo "tier=${TIER} log=${LOG}"
# the outer harness keeps the raw log and makes the one bundle, so the inner guard stays off
SUPPORT_BUNDLE_DISABLED="${BLOX7_DISABLE_SUPPORT_BUNDLE:-0}"
export BLOX7_DISABLE_SUPPORT_BUNDLE=1
set +e
# roblox can ignore SIGTERM while an engine thread is busy, so the run gets a hard limit.
# C with the present timer off is the interactive launch and stays up until the window closes
if [[ "${TIER}" == "NETWORK" ]]; then
  timeout --kill-after=5s "${BLOX7_SMOKE_TIMEOUT_S}" \
    "${BIN}" "${EXTRA[@]}" "${USER_ARGUMENTS[@]}" >"${LOG}" 2>&1
  rc=$?
  chmod 600 "${LOG}"
elif [[ "${TIER}" == "C" &&
        "${BLOX7_AUTO_EXIT_AFTER_PRESENT_MS:-}" == "0" ]]; then
  "${BIN}" "${EXTRA[@]}" "${USER_ARGUMENTS[@]}" 2>&1 | tee "${LOG}"
  rc=${PIPESTATUS[0]}
else
  timeout --kill-after=5s "${BLOX7_SMOKE_TIMEOUT_S:-120}" \
    "${BIN}" "${EXTRA[@]}" "${USER_ARGUMENTS[@]}" 2>&1 | tee "${LOG}"
  rc=${PIPESTATUS[0]}
fi
set -e

if [[ "${TIER}" != "NETWORK" ]]; then
  echo "---- markers ----"
  rg -n 'JNI_OnLoad|nativeGameGlobalInit|nativeAppBridgeV2Init|nativeAppBridgeV2Start|StartLua|Mobile\.rbxl|LuaApp|UGCGame|DeviceGL|RenderJob|Mimalloc|egl(CreateWindowSurface|MakeCurrent|SwapBuffers)|vkQueuePresentKHR|presented|typed production input|SDL readiness pointer|mouse button reached|touch event reached|lifecycle shutdown|automatic exit|abort|FATAL|startup|compat|skipping libroblox' "${LOG}" | head -180 || true
fi

if [[ "${TIER}" == "C" && "${rc}" -eq 0 ]]; then
  required_markers=(
    "[compat] legacy binary patches: disabled"
    "[compat] signal-recovery handler disabled"
    "[compat] native allocator retained; host allocator bridges disabled"
    "[window] first Roblox Vulkan frame presented"
    "[vulkan] SDL WSI adapter shut down"
    "[main] Roblox lifecycle shutdown: Stopped"
  )
  for marker in "${required_markers[@]}"; do
    if ! rg --fixed-strings --quiet "${marker}" "${LOG}"; then
      echo "Tier C readiness marker missing: ${marker}" >&2
      rc=1
    fi
  done
  # the shader-pack summary sometimes never shows up, a numbered present always does
  if ! rg --quiet '\[window\] vkQueuePresentKHR #[1-9][0-9]*' "${LOG}"; then
    echo "Tier C readiness marker missing: real Vulkan queue present" >&2
    rc=1
  fi
fi

if [[ "${TIER}" == "C" && "${rc}" -eq 0 &&
      ( "${BLOX7_FULLSCREEN_READINESS:-0}" == "1" ||
        "${BLOX7_FULLSCREEN_READINESS:-0}" == "roundtrip" ) ]]; then
  fullscreen_enter_count="$(rg --fixed-strings --count \
    "[fullscreen] SDL transition committed state=fullscreen" "${LOG}" || true)"
  fullscreen_leave_count="$(rg --fixed-strings --count \
    "[fullscreen] SDL transition committed state=windowed" "${LOG}" || true)"
  if [[ "${BLOX7_FULLSCREEN_READINESS:-0}" == "roundtrip" &&
        ( "${fullscreen_enter_count:-0}" -lt 1 ||
          "${fullscreen_leave_count:-0}" -lt 1 ) ]]; then
    echo "Tier C fullscreen roundtrip did not observe both SDL transitions" >&2
    rc=1
  fi
  fullscreen_line="$(rg --fixed-strings --line-number \
    "[fullscreen] SDL transition committed" "${LOG}" \
    | cut -d: -f1 | tail -n 1 || true)"
  if [[ -z "${fullscreen_line}" ]]; then
    echo "Tier C fullscreen readiness did not observe an SDL transition" >&2
    rc=1
  else
    post_fullscreen_present_line="$(rg --fixed-strings --line-number \
      "[window] vkQueuePresentKHR #" "${LOG}" \
      | cut -d: -f1 \
      | awk -v boundary="${fullscreen_line}" '$1 > boundary { print; exit }' \
      || true)"
    if [[ -z "${post_fullscreen_present_line}" ]]; then
      echo "Tier C fullscreen readiness did not present after F11" >&2
      rc=1
    fi
  fi

  first_out_of_date_line="$(rg --fixed-strings --line-number --max-count 1 \
    "VK_ERROR_OUT_OF_DATE_KHR" "${LOG}" | cut -d: -f1 || true)"
  last_out_of_date_line="$(rg --fixed-strings --line-number \
    "VK_ERROR_OUT_OF_DATE_KHR" "${LOG}" \
    | cut -d: -f1 | tail -n 1 || true)"
  if [[ -n "${first_out_of_date_line}" ]]; then
    recreation_line="$(rg --fixed-strings --line-number \
      "[surface] LuaApp JNI recreated" "${LOG}" \
      | cut -d: -f1 \
      | awk -v boundary="${first_out_of_date_line}" \
          '$1 > boundary { print; exit }' || true)"
    post_error_present_line="$(rg --fixed-strings --line-number \
      "[window] vkQueuePresentKHR #" "${LOG}" \
      | cut -d: -f1 \
      | awk -v boundary="${last_out_of_date_line}" \
          '$1 > boundary { print; exit }' || true)"
    if [[ -z "${recreation_line}" ]]; then
      echo "Tier C fullscreen readiness did not recreate LuaApp surface" >&2
      rc=1
    fi
    if [[ -z "${post_error_present_line}" ]]; then
      echo "Tier C fullscreen readiness did not present after the last Vulkan out-of-date result" >&2
      rc=1
    fi
  fi
fi

if [[ ("${TIER}" == "GAME" || "${TIER}" == "INPUT" ||
       "${TIER}" == "RESIZE") && "${rc}" -eq 0 ]]; then
  required_markers=(
    "[compat] legacy binary patches: disabled"
    "[compat] signal-recovery handler disabled"
    "[compat] native allocator retained; host allocator bridges disabled"
    "setStage: (stage:UGCGame)"
    "[window] first Roblox Vulkan frame presented"
    "RenderView destroyed"
    "Vulkan: Saved pipeline cache"
    "[vulkan] SDL WSI adapter shut down"
    "UgcExperienceController: finalized"
    "setStage: (stage:None)"
    "[main] Roblox lifecycle shutdown: Stopped"
  )
  for marker in "${required_markers[@]}"; do
    if ! rg --fixed-strings --quiet "${marker}" "${LOG}"; then
      echo "${TIER} readiness marker missing: ${marker}" >&2
      rc=1
    fi
  done

  forbidden_markers=(
    "nativeAppBridgeV2ResumeGameWithPlatformParams after StartGame"
    "nativeAppBridgeV2SendAppEventOnGameLoaded"
  )
  for marker in "${forbidden_markers[@]}"; do
    if rg --fixed-strings --quiet "${marker}" "${LOG}"; then
      echo "${TIER} readiness used a forbidden initial-lifecycle call: ${marker}" >&2
      rc=1
    fi
  done

  ugc_game_line="$(rg --fixed-strings --line-number --max-count 1 \
    "setStage: (stage:UGCGame)" "${LOG}" | cut -d: -f1 || true)"
  early_update_line="$(rg --fixed-strings --line-number \
    "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams" "${LOG}" \
    | cut -d: -f1 \
    | awk -v ugc="${ugc_game_line:-0}" '$1 < ugc { print; exit }' \
    || true)"
  if [[ -n "${early_update_line}" ]]; then
    echo "${TIER} readiness updated the surface before UGCGame was running" >&2
    rc=1
  fi

  previous_line=0
  ordered_markers=(
    "[engine] nativeAppBridgeV2StartGameWithParam"
    "setStage: (stage:UGCGame)"
    "[window] vkQueuePresentKHR #"
    "setStage: (stage:None)"
    "[main] Roblox lifecycle shutdown: Stopped"
  )
  for marker in "${ordered_markers[@]}"; do
    marker_line="$(rg --fixed-strings --line-number "${marker}" "${LOG}" \
      | cut -d: -f1 \
      | awk -v previous="${previous_line}" '$1 > previous { print; exit }' \
      || true)"
    if [[ -z "${marker_line}" ]]; then
      echo "${TIER} readiness order violation at marker: ${marker}" >&2
      rc=1
      break
    fi
    previous_line="${marker_line}"
  done
fi

if [[ "${TIER}" == "INPUT" && "${rc}" -eq 0 ]]; then
  if ! "${ROOT}/scripts/input_readiness_gate.sh" validate "${LOG}"; then
    rc=1
  fi
fi

if [[ "${TIER}" == "RESIZE" && "${rc}" -eq 0 ]]; then
  if ! "${ROOT}/scripts/resize_readiness_gate.sh" validate "${LOG}"; then
    rc=1
  fi
fi

if [[ ("${TIER}" == "C" || "${TIER}" == "GAME" ||
       "${TIER}" == "INPUT" || "${TIER}" == "RESIZE") &&
      "${rc}" -eq 0 ]]; then
  if ! "${ROOT}/scripts/audio_readiness_gate.sh" "${LOG}"; then
    rc=1
  fi
fi

if [[ "${TIER}" == "NETWORK" ]]; then
  if [[ "${rc}" -eq 0 ]]; then
    if ! "${ROOT}/scripts/network_readiness_gate.sh" validate \
      "${place_id}" "${LOG}"; then
      rc=1
    fi
  else
    echo "NETWORK runtime exited before readiness validation" >&2
  fi
fi

echo "exit=${rc}"
if [[ "${rc}" -ne 0 && "${SUPPORT_BUNDLE_DISABLED}" != 1 ]]; then
  support_collector="${BLOX7_SUPPORT_BUNDLE_SCRIPT:-${ROOT}/scripts/collect_support_bundle.sh}"
  if [[ "${support_collector}" == /* && -f "${support_collector}" &&
        ! -L "${support_collector}" && -x "${support_collector}" ]]; then
    support_archive="$("${support_collector}" --context readiness \
      --reason "tier-${TIER}-failed" --exit-code "${rc}" --log "${LOG}" \
      2>/dev/null || true)"
    if [[ -n "${support_archive}" ]]; then
      echo "support_bundle=${support_archive}"
    else
      echo "support bundle collection failed" >&2
    fi
  fi
fi
exit "${rc}"

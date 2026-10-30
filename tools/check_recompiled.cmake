# Fails the build when the recompiled code in RECOMPILED was not made from this checkout's config, run
# on every build (the recompiled_check target in CMakeLists.txt). Hooks live in config/us.toml and are
# compiled into the recompiled functions, so after a change to it the game builds and runs fine with
# the old code and the new hooks silently do nothing (a resumed save replayed scene 64's boss fight
# from its start for a day that way). tools/regenerate.sh writes RECOMPILED/config.sha256.
set(stamp "${RECOMPILED}/config.sha256")
set(fix "Run tools/regenerate.sh, then build again (or configure with -DCONKER_ALLOW_STALE_RECOMPILED=ON to build anyway).")
if(NOT EXISTS "${stamp}")
    message(FATAL_ERROR "${RECOMPILED} has no config.sha256: it was made by an older tools/regenerate.sh "
        "and may not have the hooks in config/us.toml. ${fix}")
endif()
file(STRINGS "${stamp}" lines)
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^([0-9a-f]+)  (.+)$")
        continue()
    endif()
    set(recorded "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    file(SHA256 "${SOURCE}/${name}" now)
    if(NOT now STREQUAL recorded)
        message(FATAL_ERROR "${name} changed since the code in ${RECOMPILED} was recompiled, so its hooks "
            "are not in this build. ${fix}")
    endif()
endforeach()

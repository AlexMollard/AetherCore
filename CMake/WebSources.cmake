# Explicit portable closure. No glob may add native services to this target.
set(AETHER_WEB_ENGINE_SOURCES
    scene/World.cpp
    scene/System.cpp
    scripting/GameplayAccess.cpp
    scripting/GameplayAbiValidation.cpp
    platform/web/WebLog.cpp
)

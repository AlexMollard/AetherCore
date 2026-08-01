#include <doctest/doctest.h>

#include <cstddef>
#include <set>
#include <string>
#include <thread>

#include "memory/MemoryScope.hpp"
#include "memory/MemoryTag.hpp"

using namespace aether::memory;

// The enum, the count and the name table all come from one X-macro list; this fails if a
// future edit adds a tag to only some of them.
TEST_CASE("Every tag has a unique name") {
    std::set<std::string> names;
    for (std::size_t i = 0; i < kMemTagCount; ++i)
    {
        names.insert(ToString(static_cast<MemTag>(i)));
    }
    CHECK(names.size() == kMemTagCount);
}

// Called from the allocation path, where faulting on a bad value would be far worse than
// reporting it vaguely.
TEST_CASE("An out-of-range tag still yields a name") {
    CHECK(std::string(ToString(static_cast<MemTag>(kMemTagCount + 40))) == "Unknown");
}

TEST_CASE("The default tag is Unknown") {
    CHECK(CurrentTag() == MemTag::Unknown);
}

// Restoring the PREVIOUS tag rather than Unknown is what makes nesting work: a texture load
// inside a render pass must leave Rendering behind, not nothing.
TEST_CASE("Scopes nest and restore the enclosing tag") {
    CHECK(CurrentTag() == MemTag::Unknown);
    {
        AE_MEM_SCOPE(MemTag::Rendering);
        CHECK(CurrentTag() == MemTag::Rendering);
        {
            AE_MEM_SCOPE(MemTag::Mesh);
            CHECK(CurrentTag() == MemTag::Mesh);
            {
                AE_MEM_SCOPE(MemTag::Texture);
                CHECK(CurrentTag() == MemTag::Texture);
            }
            CHECK(CurrentTag() == MemTag::Mesh);
        }
        CHECK(CurrentTag() == MemTag::Rendering);
    }
    CHECK(CurrentTag() == MemTag::Unknown);
}

// Two scopes in one block must not collide on a generated name.
TEST_CASE("Two scopes in the same block both compile and apply in order") {
    {
        AE_MEM_SCOPE(MemTag::Physics2D);
        CHECK(CurrentTag() == MemTag::Physics2D);
    }
    {
        AE_MEM_SCOPE(MemTag::Audio);
        CHECK(CurrentTag() == MemTag::Audio);
    }
}

// The tag is per-thread. Engine code runs on threads the CLR created, and one thread's scope
// must never attribute another thread's allocations.
TEST_CASE("The tag is thread-local and does not leak across threads") {
    AE_MEM_SCOPE(MemTag::Editor);
    CHECK(CurrentTag() == MemTag::Editor);

    MemTag observed = MemTag::Editor;
    std::thread worker([&observed] { observed = CurrentTag(); });
    worker.join();

    CHECK(observed == MemTag::Unknown);
    CHECK(CurrentTag() == MemTag::Editor);
}

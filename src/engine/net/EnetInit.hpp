#pragma once

namespace aether::net
{
	// ENet's global init is process-wide and NOT refcounted by the library. Both the
	// game transport and the editor's control server initialise it, and either may
	// outlive the other - so exactly one counter must exist for the whole process.
	// Two independent counters (one per module) would let one module's teardown call
	// enet_deinitialize() while the other is still running.
	[[nodiscard]] bool AcquireEnet();
	void ReleaseEnet();

	// Test observation only: the current number of outstanding ENet references.
	// The suite cannot observe the refcount through behaviour on Windows (the
	// underlying primitives tolerate redundant init/deinit), so it asserts on this
	// instead - nothing outside the tests should need it.
	[[nodiscard]] int EnetReferenceCount();
} // namespace aether::net

#pragma once
#include <vector>
#include "material/IMaterialSlotSink.hpp"

// In-memory slot backend for registry tests. No GPU.
class FakeSlotSink final : public aether::IMaterialSlotSink
{
public:
	explicit FakeSlotSink(std::uint32_t capacity = 8) : m_slots(capacity) {
		m_used.assign(capacity, false);
	}

	std::uint32_t AllocateSlot() override {
		for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
			if (!m_used[i]) { m_used[i] = true; ++allocCount; return i; }
		}
		return kInvalidSlot;
	}
	void FreeSlot(std::uint32_t slot) override {
		if (slot < m_slots.size()) { m_used[slot] = false; ++freeCount; }
	}
	void Write(std::uint32_t slot, const aether::GpuMaterial& m) override {
		if (slot < m_slots.size()) { m_slots[slot] = m; ++writeCount; }
	}
	std::uint32_t Capacity() const override { return static_cast<std::uint32_t>(m_slots.size()); }

	int allocCount = 0, freeCount = 0, writeCount = 0;
private:
	std::vector<aether::GpuMaterial> m_slots;
	std::vector<bool> m_used;
};

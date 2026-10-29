#include <limits>
#include <doctest/doctest.h>
#include "streaming/StreamingBudget.hpp"

using namespace aether::streaming;

TEST_CASE("Streaming ledger includes staging in CPU and enforces both limits")
{
	BudgetLedger ledger;
	Budgets limits;
	limits.cpuBytes = 100;
	limits.gpuBytes = 80;
	limits.stagingBytes = 30;
	REQUIRE(ledger.Configure(limits));
	CHECK(ledger.TryReserve(MemoryClass::CpuPending, 75));
	CHECK_FALSE(ledger.TryReserve(MemoryClass::Staging, 26));
	CHECK(ledger.TryReserve(MemoryClass::Staging, 25));
	CHECK_FALSE(ledger.TryReserve(MemoryClass::Staging, 1));
	CHECK(ledger.TryReserve(MemoryClass::GpuPending, 80));
	CHECK_FALSE(ledger.TryReserve(MemoryClass::GpuRetiring, 1));
	CHECK(ledger.Bytes(MemoryClass::Staging) == 25);
}

TEST_CASE("Streaming ledger retains retiring charges and permits transitions after shrinking")
{
	BudgetLedger ledger;
	Budgets limits;
	limits.gpuBytes = 100;
	REQUIRE(ledger.Configure(limits));
	REQUIRE(ledger.TryReserve(MemoryClass::GpuResident, 90));
	limits.gpuBytes = 40;
	REQUIRE(ledger.Configure(limits));
	CHECK(ledger.Transfer(MemoryClass::GpuResident, MemoryClass::GpuRetiring, 90));
	CHECK_FALSE(ledger.TryReserve(MemoryClass::GpuPending, 1));
	ledger.Release(MemoryClass::GpuRetiring, 90);
	CHECK(ledger.TryReserve(MemoryClass::GpuPending, 40));
}

TEST_CASE("Streaming ledger transfers are atomic and reject overflow")
{
	BudgetLedger ledger;
	Budgets limits;
	limits.cpuBytes = std::numeric_limits<std::uint64_t>::max();
	limits.gpuBytes = 4;
	limits.stagingBytes = 2;
	REQUIRE(ledger.Configure(limits));
	REQUIRE(ledger.TryReserve(MemoryClass::CpuPending, 5));
	CHECK_FALSE(ledger.Transfer(MemoryClass::CpuPending, MemoryClass::GpuPending, 5));
	CHECK_FALSE(ledger.Transfer(MemoryClass::CpuPending, MemoryClass::Staging, 3));
	CHECK(ledger.Bytes(MemoryClass::CpuPending) == 5);
	CHECK(ledger.Bytes(MemoryClass::GpuPending) == 0);
	CHECK_FALSE(ledger.TryReserve(MemoryClass::CpuPending, std::numeric_limits<std::uint64_t>::max()));
}

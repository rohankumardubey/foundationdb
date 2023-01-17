/*
 * RawTenantAccessWorkload.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2023 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbserver/workloads/workloads.actor.h"
#include "fdbclient/IClientApi.h"
#include "fdbclient/ThreadSafeTransaction.h"
#include "fdbclient/RunRYWTransaction.actor.h"
#include "fdbclient/TenantSpecialKeys.actor.h"
#include "fdbserver/Knobs.h"
#include "flow/actorcompiler.h"

struct RawTenantAccessWorkload : TestWorkload {
	static constexpr auto NAME = "RawTenantAccess";

	const Key specialKeysTenantMapPrefix = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT)
	                                           .begin.withSuffix(TenantRangeImpl::submoduleRange.begin)
	                                           .withSuffix(TenantRangeImpl::mapSubRange.begin);
	const KeyRef writeKey = "key"_sr;
	const ValueRef writeValue = "value"_sr;

	int tenantCount;
	double testDuration;

	std::set<int> lastCreatedTenants; // the index of tenant to be created if the last transaction succeed
	std::set<int> lastDeletedTenants; // the index of tenant to be deleted if the last transaction succeed
	std::map<int, int64_t> idx2Tid; // workload tenant idx to tenantId
	std::map<int64_t, int> tid2Idx; // tenant id to tenant index in this workload

	RawTenantAccessWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		tenantCount = getOption(options, "tenantCount"_sr, 1000);
		testDuration = getOption(options, "testDuration"_sr, 120.0);
	}

	Future<Void> setup(Database const& cx) override {
		if (clientId == 0 && g_network->isSimulated() && BUGGIFY) {
			IKnobCollection::getMutableGlobalKnobCollection().setKnob(
			    "max_tenants_per_cluster", KnobValueRef::create(int{ deterministicRandom()->randomInt(20, 100) }));
		}

		if (clientId == 0) {
			return _setup(cx, this);
		}
		return Void();
	}

	TenantName indexToTenantName(int index) {
		auto name = fmt::format("tenant_idx_{:06d}", index);
		return TenantName(StringRef(name));
	}

	ACTOR static Future<Void> _setup(Database cx, RawTenantAccessWorkload* self) {
		RawTenantAccessWorkload* workload = self;
		// create N tenant through special key space
		wait(runRYWTransaction(cx, [workload](Reference<ReadYourWritesTransaction> tr) {
			tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			for (int i = 0; i < workload->tenantCount; ++i) {
				tr->set(workload->specialKeysTenantMapPrefix.withSuffix(workload->indexToTenantName(i)), ""_sr);
			}
			return Future<Void>(Void());
		}));

		for (int i = 0; i < self->tenantCount; ++i) {
			self->lastCreatedTenants.insert(i);
		}
		return Void();
	}

	int64_t extractTenantId(ValueRef value) {
		int64_t id;
		json_spirit::mValue jsonObject;
		json_spirit::read_string(value.toString(), jsonObject);
		JSONDoc jsonDoc(jsonObject);
		jsonDoc.get("id", id);
		return id;
	}

	void eraseDeletedTenants() {
		for (auto idx : lastDeletedTenants) {
			auto tid = tid2Idx.at(idx);
			tid2Idx.erase(tid);
			idx2Tid.erase(idx);
		}
		lastDeletedTenants.clear();
	}

	ACTOR static Future<Void> applyTenantChanges(Database cx, RawTenantAccessWorkload* self) {
		// erase deleted tenants
		self->eraseDeletedTenants();

		// load the tenant id of new tenants
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
		loop {
			tr->reset();
			try {
				tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
				state std::set<int>::const_iterator it = self->lastCreatedTenants.cbegin();
				while (it != self->lastCreatedTenants.end()) {
					Key key = self->specialKeysTenantMapPrefix.withSuffix(self->indexToTenantName(*it));
					Optional<Value> value = wait(tr->get(key));
					ASSERT(value.present());
					auto id = self->extractTenantId(value.get());
					self->idx2Tid[*it] = id;
					self->tid2Idx[id] = *it;
					++it;
				}
				break;
			} catch (Error& e) {
				wait(tr->onError(e));
			}
		}

		self->lastCreatedTenants.clear();
		return Void();
	}

	Future<Void> start(Database const& cx) override {
		if (clientId == 0) {
			return ready(timeout(_start(cx, this), testDuration));
		}
		return Void();
	}

	int predictTenantCount() const { return idx2Tid.size() + lastCreatedTenants.size() - lastDeletedTenants.size(); }

	void createNewTenant(Reference<ReadYourWritesTransaction> tr) {
		ASSERT_LT(predictTenantCount(), tenantCount);
		int tenantIdx = deterministicRandom()->randomInt(0, tenantCount);
		// find the nearest non-existed tenant
		while (idx2Tid.count(tenantIdx) && lastCreatedTenants.count(tenantIdx)) {
			tenantIdx++;
			if (tenantIdx == tenantCount) {
				tenantIdx = 0;
			}
		}

		tr->set(specialKeysTenantMapPrefix.withSuffix(indexToTenantName(tenantIdx)), ""_sr);
		lastCreatedTenants.insert(tenantIdx);
		lastDeletedTenants.erase(tenantIdx);
	}

	void deleteExistedTenant(Reference<ReadYourWritesTransaction> tr) {
		ASSERT_GT(predictTenantCount(), 0);
		int tenantIdx = deterministicRandom()->randomInt(0, tenantCount);
		// find the nearest existed tenant
		while (true) {
			if ((idx2Tid.count(tenantIdx) || lastCreatedTenants.count(tenantIdx)) &&
			    !lastDeletedTenants.count(tenantIdx)) {
				break;
			}
			tenantIdx++;
			if (tenantIdx == tenantCount) {
				tenantIdx = 0;
			}
		}

		Key key = specialKeysTenantMapPrefix.withSuffix(indexToTenantName(tenantIdx));
		tr->clear(key);
		lastCreatedTenants.erase(tenantIdx);
		lastDeletedTenants.insert(tenantIdx);
	}

	void writeToExistedTenant(Reference<ReadYourWritesTransaction> tr) {
		ASSERT_GT(idx2Tid.size(), 0);
		// determine the tenant to write
		int tenantIdx = deterministicRandom()->randomInt(0, tenantCount);
		auto it = idx2Tid.lower_bound(tenantIdx);
		if (it == idx2Tid.end()) {
			tenantIdx = idx2Tid.begin()->first;
		} else {
			tenantIdx = it->first;
		}

		// write the raw data
		int64_t tenantId = idx2Tid.at(tenantIdx);
		Key prefix = TenantAPI::idToPrefix(tenantId);
		tr->set(prefix.withSuffix(writeKey), writeValue);
	}

	void writeToInvalidTenant(Reference<ReadYourWritesTransaction> tr) {
		ASSERT_LT(predictTenantCount(), tenantCount);
		// determine the invalid tenant id
		int64_t tenantId = TenantInfo::INVALID_TENANT;
		if (deterministicRandom()->coinflip() && lastDeletedTenants.size() > 0) {
			// choose the tenant deleted in the same transaction
			tenantId = idx2Tid.at(*lastDeletedTenants.begin());
		} else {
			// randomly generate a tenant id
			do {
				tenantId = deterministicRandom()->randomInt64(0, std::numeric_limits<int64_t>::max());
			} while (tid2Idx.count(tenantId));
		}
		ASSERT_GE(tenantId, 0);

		// write to invalid tenant
		Key prefix = TenantAPI::idToPrefix(tenantId);
		tr->set(prefix.withSuffix(writeKey), writeValue);
	}

	ACTOR static Future<Void> randomTenantTransaction(Database cx, RawTenantAccessWorkload* self) {
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
		state UID traceId = deterministicRandom()->randomUniqueID();
		state bool illegalAccess = false;
		state bool illegalAccessCaught = false;

		loop {
			tr->reset();
			try {
				tr->setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
				tr->setOption(FDBTransactionOptions::RAW_ACCESS);
				// the transaction will randomly run 10 ops
				state int i = 0;
				for (; i < 10; ++i) {
					int op = deterministicRandom()->randomInt(0, 4);
					if (op == 0 && self->predictTenantCount() < self->tenantCount) {
						// whether to create a new Tenant
						self->createNewTenant(tr);
					} else if (op == 1 && self->predictTenantCount() > 0) {
						// whether to delete a existed tenant
						self->deleteExistedTenant(tr);
					} else if (op == 2 && self->predictTenantCount() < self->tenantCount) {
						// whether to write to a non-existed tenant
						self->writeToInvalidTenant(tr);
						illegalAccess = true;
					} else if (op == 3 && self->idx2Tid.size() > 0) {
						// whether to write to a existed tenant
						self->writeToExistedTenant(tr);
					}
				}

				wait(tr->commit());
				break;
			} catch (Error& e) {
				if (e.code() == error_code_illegal_tenant_access) {
					illegalAccessCaught = true;
				}
				wait(tr->onError(e));
			}
		}
		ASSERT_EQ(illegalAccessCaught, illegalAccess);

		return Void();
	}

	// clear tenant data to make sure the random tenant deletions are success
	ACTOR static Future<Void> clearAllTenantData(Database cx, RawTenantAccessWorkload* self) {
		RawTenantAccessWorkload* workload = self;
		wait(runRYWTransaction(cx, [workload](Reference<ReadYourWritesTransaction> tr) {
			tr->setOption(FDBTransactionOptions::RAW_ACCESS);
			for (auto [tid, _] : workload->tid2Idx) {
				Key prefix = TenantAPI::idToPrefix(tid);
				tr->clear(prefix.withSuffix(workload->writeKey));
			}
			return Future<Void>(Void());
		}));
		return Void();
	}

	ACTOR static Future<Void> _start(Database cx, RawTenantAccessWorkload* self) {
		loop {
			wait(applyTenantChanges(cx, self));
			wait(clearAllTenantData(cx, self));
			wait(randomTenantTransaction(cx, self));
			wait(delay(0.5));
		}
	}

	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<RawTenantAccessWorkload> RawTenantAccessWorkload;
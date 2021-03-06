/**
 * Copyright (c) 2016-present, Facebook, Inc.
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

#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>
#include "caffe2/core/common.h"
#include "caffe2/core/net.h"
#include "caffe2/core/net_dag.h"
#include "caffe2/core/net_simple.h"
#include "caffe2/core/observer.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/registry.h"
#include "caffe2/core/scope_guard.h"

namespace caffe2 {

namespace {

static std::atomic<int> counter;

template <class T>
class DummyObserver final : public ObserverBase<T> {
 public:
  explicit DummyObserver<T>(T* subject_) : ObserverBase<T>(subject_) {}
  bool Start() override;
  bool Stop() override;

  ~DummyObserver() {}
};

template <>
bool DummyObserver<NetBase>::Start() {
  vector<OperatorBase*> operators = subject_->GetOperators();
  for (auto& op : operators) {
    op->AttachObserver(caffe2::make_unique<DummyObserver<OperatorBase>>(op));
  }
  counter.fetch_add(1000);
  return true;
}

template <>
bool DummyObserver<OperatorBase>::Start() {
  counter.fetch_add(100);
  return true;
}

template <>
bool DummyObserver<NetBase>::Stop() {
  counter.fetch_add(10);
  return true;
}

template <>
bool DummyObserver<OperatorBase>::Stop() {
  counter.fetch_add(1);
  return true;
}

class ObsTestDummyOp final : public OperatorBase {
 public:
  using OperatorBase::OperatorBase;
  bool Run(int /* unused */) override {
    StartAllObservers();
    StopAllObservers();
    return true;
  }
};

REGISTER_CPU_OPERATOR(ObsTestDummy, ObsTestDummyOp);
REGISTER_CUDA_OPERATOR(ObsTestDummy, ObsTestDummyOp);

OPERATOR_SCHEMA(ObsTestDummy)
    .NumInputs(0, INT_MAX)
    .NumOutputs(0, INT_MAX)
    .AllowInplace({{0, 0}, {1, 1}});

unique_ptr<NetBase> CreateNetTestHelper(Workspace* ws, bool isDAG = false) {
  NetDef net_def;
  if (isDAG) {
    net_def.set_type("dag");
  }
  {
    auto& op = *(net_def.add_op());
    op.set_type("ObsTestDummy");
    op.add_input("in");
    op.add_output("hidden");
  }
  {
    auto& op = *(net_def.add_op());
    op.set_type("ObsTestDummy");
    op.add_input("hidden");
    op.add_output("out");
  }
  net_def.add_external_input("in");
  net_def.add_external_output("out");

  return CreateNet(net_def, ws);
}
}

TEST(ObserverTest, TestNotify) {
  auto count_before = counter.load();
  Workspace ws;
  ws.CreateBlob("in");
  NetDef net_def;
  unique_ptr<NetBase> net(CreateNetTestHelper(&ws));
  EXPECT_EQ(caffe2::dynamic_cast_if_rtti<SimpleNet*>(net.get()), net.get());
  unique_ptr<DummyObserver<NetBase>> net_ob =
      make_unique<DummyObserver<NetBase>>(net.get());
  net.get()->AttachObserver(std::move(net_ob));
  net.get()->Run();
  auto count_after = counter.load();
  EXPECT_EQ(1212, count_after - count_before);
}

TEST(ObserverTest, TestUniqueMap) {
  auto count_before = counter.load();
  Workspace ws;
  ws.CreateBlob("in");
  NetDef net_def;
  unique_ptr<NetBase> net(CreateNetTestHelper(&ws));
  EXPECT_EQ(caffe2::dynamic_cast_if_rtti<SimpleNet*>(net.get()), net.get());
  unique_ptr<DummyObserver<NetBase>> net_ob =
      make_unique<DummyObserver<NetBase>>(net.get());
  auto* ref = net.get()->AttachObserver(std::move(net_ob));
  net.get()->Run();
  unique_ptr<Observable<NetBase>::Observer> test =
      net.get()->DetachObserver(ref);
  auto count_after = counter.load();
  EXPECT_EQ(1212, count_after - count_before);
}

TEST(ObserverTest, TestNotifyAfterDetach) {
  auto count_before = counter.load();
  Workspace ws;
  ws.CreateBlob("in");
  NetDef net_def;
  unique_ptr<NetBase> net(CreateNetTestHelper(&ws));
  unique_ptr<DummyObserver<NetBase>> net_ob =
      make_unique<DummyObserver<NetBase>>(net.get());
  auto* ob = net.get()->AttachObserver(std::move(net_ob));
  net.get()->DetachObserver(ob);
  net.get()->Run();
  auto count_after = counter.load();
  EXPECT_EQ(0, count_after - count_before);
}

TEST(ObserverTest, TestDAGNetBase) {
  auto count_before = counter.load();
  Workspace ws;
  ws.CreateBlob("in");
  NetDef net_def;
  unique_ptr<NetBase> net(CreateNetTestHelper(&ws, true));
  EXPECT_EQ(caffe2::dynamic_cast_if_rtti<DAGNetBase*>(net.get()), net.get());
  unique_ptr<DummyObserver<NetBase>> net_ob =
      make_unique<DummyObserver<NetBase>>(net.get());
  net.get()->AttachObserver(std::move(net_ob));
  net.get()->Run();
  auto count_after = counter.load();
  EXPECT_EQ(1212, count_after - count_before);
}

TEST(ObserverTest, TestMultipleNetBase) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef net_def;
  unique_ptr<NetBase> net(CreateNetTestHelper(&ws, true));
  EXPECT_EQ(caffe2::dynamic_cast_if_rtti<NetBase*>(net.get()), net.get());

  // There may be some default observers
  const size_t prev_num = net.get()->NumObservers();
  const int num_tests = 100;
  vector<const Observable<NetBase>::Observer*> observers;
  for (int i = 0; i < num_tests; ++i) {
    unique_ptr<DummyObserver<NetBase>> net_ob =
        make_unique<DummyObserver<NetBase>>(net.get());
    observers.emplace_back(net.get()->AttachObserver(std::move(net_ob)));
  }

  net.get()->Run();

  for (const auto& observer : observers) {
    net.get()->DetachObserver(observer);
  }

  EXPECT_EQ(net.get()->NumObservers(), prev_num);
}
} // namespace caffe2

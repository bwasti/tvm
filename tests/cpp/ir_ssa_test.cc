/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <dmlc/logging.h>
#include <gtest/gtest.h>
#include <tvm/tvm.h>
#include <tvm/ir_pass.h>


TEST(IRSSA, Convert) {
  using namespace HalideIR::Internal;
  using namespace tvm;
  Var x("x"), y;
  Expr let = Let::make(x, 1, x + 1);

  auto z = Evaluate::make(let + let);
  CHECK(!ir::VerifySSA(z));
  auto z_ssa = ir::ConvertSSA(z);
  CHECK(ir::VerifySSA(z_ssa));
}

TEST(IRSSA, Basic) {
  using namespace HalideIR::Internal;
  using namespace tvm;
  Var x("x"), y;
  auto z = Evaluate::make(x + y);
  CHECK(ir::VerifySSA(z));
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  return RUN_ALL_TESTS();
}

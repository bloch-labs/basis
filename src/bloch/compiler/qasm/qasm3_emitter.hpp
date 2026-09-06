// Copyright 2025-2026 Akshay Pal (https://bloch-labs.com)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>

namespace bloch::compiler {

struct Program;

class Qasm3Emitter {
   public:
    [[nodiscard]] static bool requires_structured_emission(const Program& program) noexcept;
    [[nodiscard]] std::string emit(const Program& program) const;
};

}  // namespace bloch::compiler

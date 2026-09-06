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

#include "bloch/runtime/qasm_simulator.hpp"

#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string_view>

namespace bloch::runtime {

using support::BlochError;
using support::ErrorCategory;

static std::mt19937 rng{std::random_device{}()};
// TODO(REFACTOR): inject RNG via a Strategy/adapter so simulator is
// deterministic under test and replaceable by other random sources.

int QasmSimulator::allocateQubit() {
    // Grow the state by a factor of two, keeping existing amplitudes
    // in the |...0> subspace and zeroing the |...1> subspace.
    int index = m_qubits++;
    if (index >= static_cast<int>(m_measured.size()))
        m_measured.resize(index + 1, false);
    else
        m_measured[index] = false;
    std::vector<std::complex<double>> newState(m_state.size() * 2);
    for (size_t i = 0; i < m_state.size(); ++i) {
        newState[i] = m_state[i];
        newState[i + m_state.size()] = 0;
    }
    m_state.swap(newState);
    return index;
}

// REFACTOR: Consider a small Instruction/Gate registry (Command pattern)
// so gate matrices + logging strings live in data tables instead of one
// function per gate; would shrink interface and simplify adding new ops.
void QasmSimulator::applySingleQubitGate(int q, const std::array<std::complex<double>, 4>& m) {
    ensureQubitActive(q);
    // Standard blocked application over basis pairs differing at bit q.
    size_t step = size_t{1} << q;
    size_t size = m_state.size();
    for (size_t i = 0; i < size; i += 2 * step) {
        for (size_t j = 0; j < step; ++j) {
            size_t idx0 = i + j;
            size_t idx1 = idx0 + step;
            auto a0 = m_state[idx0];
            auto a1 = m_state[idx1];
            m_state[idx0] = m[0] * a0 + m[1] * a1;
            m_state[idx1] = m[2] * a0 + m[3] * a1;
        }
    }
}

void QasmSimulator::h(int q) {
    const std::array<std::complex<double>, 4> m{1 / std::sqrt(2.0), 1 / std::sqrt(2.0),
                                                1 / std::sqrt(2.0), -1 / std::sqrt(2.0)};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::H, q, 0, 0.0});
}

void QasmSimulator::x(int q) {
    const std::array<std::complex<double>, 4> m{0, 1, 1, 0};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::X, q, 0, 0.0});
}

void QasmSimulator::y(int q) {
    const std::array<std::complex<double>, 4> m{0.0, std::complex<double>(0, -1),
                                                std::complex<double>(0, 1), 0.0};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::Y, q, 0, 0.0});
}

void QasmSimulator::z(int q) {
    const std::array<std::complex<double>, 4> m{1.0, 0.0, 0.0, -1.0};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::Z, q, 0, 0.0});
}

void QasmSimulator::rx(int q, double t) {
    double ct = std::cos(t / 2);
    double st = std::sin(t / 2);
    const std::array<std::complex<double>, 4> m{ct, std::complex<double>(0, -st),
                                                std::complex<double>(0, -st), ct};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::Rx, q, 0, t});
}

void QasmSimulator::ry(int q, double t) {
    double ct = std::cos(t / 2);
    double st = std::sin(t / 2);
    const std::array<std::complex<double>, 4> m{ct, -st, st, ct};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::Ry, q, 0, t});
}

void QasmSimulator::rz(int q, double t) {
    std::complex<double> epos = std::exp(std::complex<double>(0, -t / 2));
    std::complex<double> eneg = std::exp(std::complex<double>(0, t / 2));
    const std::array<std::complex<double>, 4> m{epos, 0.0, 0.0, eneg};
    applySingleQubitGate(q, m);
    if (m_logOps)
        operations_.push_back({OperationType::Rz, q, 0, t});
}

void QasmSimulator::cx(int control, int target) {
    ensureQubitActive(control);
    ensureQubitActive(target);
    // Swap amplitudes where control is 1 and target is 0 to flip target,
    // iterating only the affected subspace to avoid per-index branching.
    int low = std::min(control, target);
    int high = std::max(control, target);
    size_t lowBit = size_t{1} << low;
    size_t highBit = size_t{1} << high;
    size_t blockSize = size_t{1} << (high + 1);  // chunk where bits above 'high' are fixed
    size_t lowSpan = lowBit;                     // combinations for bits below 'low'
    size_t betweenSpan = (high > low + 1) ? (size_t{1} << (high - low - 1)) : size_t{1};
    bool controlIsLow = control == low;

    for (size_t block = 0; block < m_state.size(); block += blockSize) {
        for (size_t between = 0; between < betweenSpan; ++between) {
            size_t mid = between << (low + 1);  // bits between low and high
            for (size_t lowOffset = 0; lowOffset < lowSpan; ++lowOffset) {
                size_t base = block | mid | lowOffset;  // control/target bits currently 0
                size_t idx0 = controlIsLow ? (base | lowBit) : (base | highBit);
                size_t idx1 = controlIsLow ? (idx0 | highBit) : (idx0 | lowBit);
                std::swap(m_state[idx0], m_state[idx1]);
            }
        }
    }
    if (m_logOps)
        operations_.push_back({OperationType::Cx, control, target, 0.0});
}

void QasmSimulator::reset(int q) {
    if (q < 0 || q >= m_qubits) {
        throw BlochError(ErrorCategory::Runtime, 0, 0,
                         "qubit index " + std::to_string(q) + " is out of range");
    }
    if (q >= 0 && q < static_cast<int>(m_measured.size()))
        m_measured[q] = false;
    // Put qubit q into |0>.
    // If the state already has amplitude in the |...0> subspace, zero the |...1> subspace
    // and renormalize. If all amplitude is in |...1>, deterministically move it into
    // the |...0> subspace (equivalent to an X on a measured |1>), avoiding NaNs.
    size_t bit = size_t{1} << q;
    double norm0 = 0.0;
    for (size_t i = 0; i < m_state.size(); ++i) {
        if (!(i & bit))
            norm0 += std::norm(m_state[i]);
    }

    if (norm0 == 0.0) {
        // All amplitude is in the |...1> subspace: swap it into |...0>.
        for (size_t i = 0; i < m_state.size(); ++i) {
            if (i & bit) {
                size_t j = i ^ bit;  // flip target bit to 0
                m_state[j] = m_state[i];
                m_state[i] = 0.0;
            }
        }
    } else {
        // Zero |...1> and renormalize |...0>
        double inv = 1.0 / std::sqrt(norm0);
        for (size_t i = 0; i < m_state.size(); ++i) {
            if (i & bit) {
                m_state[i] = 0.0;
            } else {
                m_state[i] *= inv;
            }
        }
    }

    if (m_logOps)
        operations_.push_back({OperationType::Reset, q, 0, 0.0});
}

int QasmSimulator::measure(int q) {
    ensureQubitActive(q);
    // Compute probability of |1>, sample, and collapse the state accordingly.
    size_t bit = size_t{1} << q;
    double p1 = 0;
    for (size_t i = 0; i < m_state.size(); ++i)
        if (i & bit)
            p1 += std::norm(m_state[i]);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng);
    int res = r < p1 ? 1 : 0;
    double norm = std::sqrt(res ? p1 : 1 - p1);
    for (size_t i = 0; i < m_state.size(); ++i) {
        if (((i & bit) ? 1 : 0) != res)
            m_state[i] = 0;
        else
            m_state[i] /= norm;
    }
    if (m_logOps)
        operations_.push_back({OperationType::Measure, q, 0, 0.0});
    if (q >= 0 && q < static_cast<int>(m_measured.size()))
        m_measured[q] = true;
    return res;
}

std::string QasmSimulator::getQasm(QasmVersion version) const {
    std::string output;
    output.reserve(96 + operations_.size() * 24);

    if (version == QasmVersion::OpenQasm2) {
        output.append("OPENQASM 2.0;\ninclude \"qelib1.inc\";\n");
        if (m_qubits > 0) {
            output.append("qreg q[").append(std::to_string(m_qubits));
            output.append("];\ncreg c[").append(std::to_string(m_qubits)).append("];\n");
        }
    } else {
        output.append("OPENQASM 3.0;\ninclude \"stdgates.inc\";\n");
        if (m_qubits > 0) {
            output.append("qubit[").append(std::to_string(m_qubits));
            output.append("] q;\nbit[").append(std::to_string(m_qubits)).append("] c;\n");
        }
    }

    const auto append_qubit = [&output](int qubit) {
        output.append("q[").append(std::to_string(qubit)).append("]");
    };
    const auto append_gate = [&output, &append_qubit](std::string_view gate, int qubit) {
        output.append(gate).append(" ");
        append_qubit(qubit);
        output.append(";\n");
    };
    const auto append_rotation = [&output, &append_qubit](std::string_view gate, int qubit,
                                                          double angle) {
        output.append(gate).append("(").append(std::to_string(angle)).append(") ");
        append_qubit(qubit);
        output.append(";\n");
    };

    for (const auto& operation : operations_) {
        switch (operation.type) {
            case OperationType::H:
                append_gate("h", operation.first_qubit);
                break;
            case OperationType::X:
                append_gate("x", operation.first_qubit);
                break;
            case OperationType::Y:
                append_gate("y", operation.first_qubit);
                break;
            case OperationType::Z:
                append_gate("z", operation.first_qubit);
                break;
            case OperationType::Rx:
                append_rotation("rx", operation.first_qubit, operation.angle);
                break;
            case OperationType::Ry:
                append_rotation("ry", operation.first_qubit, operation.angle);
                break;
            case OperationType::Rz:
                append_rotation("rz", operation.first_qubit, operation.angle);
                break;
            case OperationType::Cx:
                output.append("cx ");
                append_qubit(operation.first_qubit);
                output.append(",");
                append_qubit(operation.second_qubit);
                output.append(";\n");
                break;
            case OperationType::Reset:
                append_gate("reset", operation.first_qubit);
                break;
            case OperationType::Measure:
                if (version == QasmVersion::OpenQasm3) {
                    output.append("c[").append(std::to_string(operation.first_qubit));
                    output.append("] = measure ");
                    append_qubit(operation.first_qubit);
                    output.append(";\n");
                } else {
                    output.append("measure ");
                    append_qubit(operation.first_qubit);
                    output.append(" -> c[").append(std::to_string(operation.first_qubit));
                    output.append("];\n");
                }
                break;
        }
    }
    return output;
}

void QasmSimulator::ensureQubitActive(int q) const {
    if (q < 0 || q >= m_qubits) {
        throw BlochError(ErrorCategory::Runtime, 0, 0,
                         "qubit index " + std::to_string(q) + " is out of range");
    }
    if (q < static_cast<int>(m_measured.size()) && m_measured[q]) {
        throw BlochError(ErrorCategory::Runtime, 0, 0,
                         "cannot operate on measured qubit q[" + std::to_string(q) + "]");
    }
}
}  // namespace bloch::runtime

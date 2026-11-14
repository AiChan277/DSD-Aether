#include "ui/params/param_registry.hpp"

#include <algorithm>
#include <cmath>

namespace aether::ui {

namespace {

constexpr bool verify_param_registry_table() noexcept {
    for (size_t i = 0; i < kParamCount; ++i) {
        if (static_cast<size_t>(kParamSpecs[i].pid) != i) {
            return false;
        }
        if (kParamSpecs[i].min_val > kParamSpecs[i].max_val) {
            return false;
        }
        if (kParamSpecs[i].default_val < kParamSpecs[i].min_val ||
            kParamSpecs[i].default_val > kParamSpecs[i].max_val) {
            return false;
        }
    }
    return true;
}

static_assert(verify_param_registry_table(), "kParamSpecs order or default bounds invalid");

} // namespace

std::span<const ParamSpec> all_param_specs() noexcept {
    return kParamSpecs;
}

const ParamSpec* find_param_by_id(std::string_view id) noexcept {
    for (const auto& s : kParamSpecs) {
        if (id == s.id) {
            return &s;
        }
    }
    return nullptr;
}

double clamp_param_value(const ParamSpec& spec, double v) noexcept {
    if (!std::isfinite(v)) {
        return spec.default_val;
    }
    double clamped = std::clamp(v, spec.min_val, spec.max_val);
    if (spec.type == ParamType::Bool) {
        clamped = (clamped >= 0.5) ? 1.0 : 0.0;
    } else if (spec.type == ParamType::Int || spec.type == ParamType::Enum) {
        clamped = std::round(clamped);
    }
    return clamped;
}

const char* param_owner_name(ParamOwner owner) noexcept {
    switch (owner) {
    case ParamOwner::T1:
        return "T1";
    case ParamOwner::T2:
        return "T2";
    case ParamOwner::T3:
        return "T3";
    case ParamOwner::T4:
        return "T4";
    case ParamOwner::App:
        return "App";
    }
    return "App";
}

const char* param_type_name(ParamType type) noexcept {
    switch (type) {
    case ParamType::Float:
        return "float";
    case ParamType::Int:
        return "int";
    case ParamType::Bool:
        return "bool";
    case ParamType::Enum:
        return "enum";
    }
    return "float";
}

void ParamStore::reset_all_to_defaults() noexcept {
    for (size_t i = 0; i < kParamCount; ++i) {
        values_[i] = kParamSpecs[i].default_val;
    }
    ++epoch_;
}

void ParamStore::reset_project_to_defaults() noexcept {
    for (size_t i = 0; i < kParamCount; ++i) {
        if ((kParamSpecs[i].flags & PARAM_FLAG_PROJECT_SCOPE) != 0u) {
            values_[i] = kParamSpecs[i].default_val;
        }
    }
    ++epoch_;
}

bool ParamStore::set(ParamId id, double val) noexcept {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= kParamCount) {
        return false;
    }
    const double next = clamp_param_value(kParamSpecs[idx], val);
    if (std::fabs(next - values_[idx]) <= 1e-9) {
        return false;
    }
    values_[idx] = next;
    ++epoch_;
    return true;
}

bool ParamStore::set_by_id(std::string_view id, double val) noexcept {
    const ParamSpec* spec = find_param_by_id(id);
    if (spec == nullptr) {
        return false;
    }
    return set(spec->pid, val);
}

bool ParamStore::is_modified(ParamId id) const noexcept {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= kParamCount) {
        return false;
    }
    return std::fabs(values_[idx] - kParamSpecs[idx].default_val) > 1e-6;
}

} // namespace aether::ui

/*
    pybind11/stl.h: Transparent conversion for STL data types

    Copyright (c) 2016 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE file.
*/

#pragma once

#include "pybind11.h"
#include "detail/common.h"

#include <deque>
#include <initializer_list>
#include <list>
#include <map>
#include <ostream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>

// See `detail/common.h` for implementation of these guards.
#if defined(PYBIND11_HAS_OPTIONAL)
#    include <optional>
#elif defined(PYBIND11_HAS_EXP_OPTIONAL)
#    include <experimental/optional>
#endif

#if defined(PYBIND11_HAS_VARIANT)
#    include <variant>
#endif

PYBIND11_NAMESPACE_BEGIN(PYBIND11_NAMESPACE)
PYBIND11_NAMESPACE_BEGIN(detail)

//
// Begin: Equivalent of
//        https://github.com/google/clif/blob/ae4eee1de07cdf115c0c9bf9fec9ff28efce6f6c/clif/python/runtime.cc#L388-L438
/*
The three `PyObjectTypeIsConvertibleTo*()` functions below are
the result of converging the behaviors of pybind11 and PyCLIF
(http://github.com/google/clif).

Originally PyCLIF was extremely far on the permissive side of the spectrum,
while pybind11 was very far on the strict side. Originally PyCLIF accepted any
Python iterable as input for a C++ `vector`/`set`/`map` argument, as long as
the elements were convertible. The obvious (in hindsight) problem was that
any empty Python iterable could be passed to any of these C++ types, e.g. `{}`
was accpeted for C++ `vector`/`set` arguments, or `[]` for C++ `map` arguments.

The functions below strike a practical permissive-vs-strict compromise,
informed by tens of thousands of use cases in the wild. A main objective is
to prevent accidents and improve readability:

- Python literals must match the C++ types.

- For C++ `set`: The potentially reducing conversion from a Python sequence
  (e.g. Python `list` or `tuple`) to a C++ `set` must be explicit, by going
  through a Python `set`.

- However, a Python `set` can still be passed to a C++ `vector`. The rationale
  is that this conversion is not reducing. Implicit conversions of this kind
  are also fairly commonly used, therefore enforcing explicit conversions
  would have an unfavorable cost : benefit ratio; more sloppily speaking,
  such an enforcement would be more annoying than helpful.
*/

inline bool PyObjectIsInstanceWithOneOfTpNames(PyObject *obj,
                                               std::initializer_list<const char *> tp_names) {
    if (PyType_Check(obj)) {
        return false;
    }
    const char *obj_tp_name = Py_TYPE(obj)->tp_name;
    for (const auto *tp_name : tp_names) {
        if (std::strcmp(obj_tp_name, tp_name) == 0) {
            return true;
        }
    }
    return false;
}

inline bool PyObjectTypeIsConvertibleToStdVector(PyObject *obj) {
    if (PySequence_Check(obj) != 0) {
        return !PyUnicode_Check(obj) && !PyBytes_Check(obj);
    }
    return (PyGen_Check(obj) != 0) || (PyAnySet_Check(obj) != 0)
           || PyObjectIsInstanceWithOneOfTpNames(
               obj, {"dict_keys", "dict_values", "dict_items", "map", "zip"});
}

inline bool PyObjectTypeIsConvertibleToStdSet(PyObject *obj) {
    return (PyAnySet_Check(obj) != 0) || PyObjectIsInstanceWithOneOfTpNames(obj, {"dict_keys"});
}

inline bool PyObjectTypeIsConvertibleToStdMap(PyObject *obj) {
    if (PyDict_Check(obj)) {
        return true;
    }
    // Implicit requirement in the conditions below:
    // A type with `.__getitem__()` & `.items()` methods must implement these
    // to be compatible with https://docs.python.org/3/c-api/mapping.html
    if (PyMapping_Check(obj) == 0) {
        return false;
    }
    PyObject *items = PyObject_GetAttrString(obj, "items");
    if (items == nullptr) {
        PyErr_Clear();
        return false;
    }
    bool is_convertible = (PyCallable_Check(items) != 0);
    Py_DECREF(items);
    return is_convertible;
}

//
// End: Equivalent of clif/python/runtime.cc
//

/// Extracts an const lvalue reference or rvalue reference for U based on the type of T (e.g. for
/// forwarding a container element).  Typically used indirect via forwarded_type(), below.
template <typename T, typename U>
using forwarded_type = conditional_t<std::is_lvalue_reference<T>::value,
                                     remove_reference_t<U> &,
                                     remove_reference_t<U> &&>;

/// Forwards a value U as rvalue or lvalue according to whether T is rvalue or lvalue; typically
/// used for forwarding a container's elements.
template <typename T, typename U>
constexpr forwarded_type<T, U> forward_like(U &&u) {
    return std::forward<detail::forwarded_type<T, U>>(std::forward<U>(u));
}

// Checks if a container has a STL style reserve method.
// This will only return true for a `reserve()` with a `void` return.
template <typename C>
using has_reserve_method = std::is_same<decltype(std::declval<C>().reserve(0)), void>;

template <typename Type, typename Key>
struct set_caster {
    using type = Type;
    using key_conv = make_caster<Key>;

private:
    template <typename T = Type, enable_if_t<has_reserve_method<T>::value, int> = 0>
    void reserve_maybe(const anyset &s, Type *) {
        value.reserve(s.size());
    }
    void reserve_maybe(const anyset &, void *) {}

    bool convert_iterable(const iterable &itbl, bool convert) {
        for (auto it : itbl) {
            key_conv conv;
            if (!conv.load(it, convert)) {
                return false;
            }
            value.insert(cast_op<Key &&>(std::move(conv)));
        }
        return true;
    }

    bool convert_anyset(anyset s, bool convert) {
        value.clear();
        reserve_maybe(s, &value);
        return convert_iterable(s, convert);
    }

public:
    bool load(handle src, bool convert) {
        if (!PyObjectTypeIsConvertibleToStdSet(src.ptr())) {
            return false;
        }
        if (isinstance<anyset>(src)) {
            value.clear();
            return convert_anyset(reinterpret_borrow<anyset>(src), convert);
        }
        if (!convert) {
            return false;
        }
        assert(isinstance<iterable>(src));
        value.clear();
        return convert_iterable(reinterpret_borrow<iterable>(src), convert);
    }

    template <typename T>
    static handle cast(T &&src, const return_value_policy_pack &rvpp, handle parent) {
        return_value_policy_pack rvpp_local = rvpp.get(0);
        if (!std::is_lvalue_reference<T>::value) {
            rvpp_local = rvpp_local.override_policy(return_value_policy_override<Key>::policy);
        }
        pybind11::set s;
        for (auto &&value : src) {
            auto value_ = reinterpret_steal<object>(
                key_conv::cast(detail::forward_like<T>(value), rvpp_local, parent));
            if (!value_ || !s.add(std::move(value_))) {
                return handle();
            }
        }
        return s.release();
    }

    PYBIND11_TYPE_CASTER_RVPP(type, const_name("Set[") + key_conv::name + const_name("]"));
};

template <typename Type, typename Key, typename Value>
struct map_caster {
    using key_conv = make_caster<Key>;
    using value_conv = make_caster<Value>;

private:
    template <typename T = Type, enable_if_t<has_reserve_method<T>::value, int> = 0>
    void reserve_maybe(const dict &d, Type *) {
        value.reserve(d.size());
    }
    void reserve_maybe(const dict &, void *) {}

    bool convert_elements(const dict &d, bool convert) {
        value.clear();
        reserve_maybe(d, &value);
        for (auto it : d) {
            key_conv kconv;
            value_conv vconv;
            if (!kconv.load(it.first.ptr(), convert) || !vconv.load(it.second.ptr(), convert)) {
                return false;
            }
            value.emplace(cast_op<Key &&>(std::move(kconv)), cast_op<Value &&>(std::move(vconv)));
        }
        return true;
    }

public:
    bool load(handle src, bool convert) {
        if (!PyObjectTypeIsConvertibleToStdMap(src.ptr())) {
            return false;
        }
        if (isinstance<dict>(src)) {
            return convert_elements(reinterpret_borrow<dict>(src), convert);
        }
        if (!convert) {
            return false;
        }
        auto items = reinterpret_steal<object>(PyMapping_Items(src.ptr()));
        if (!items) {
            throw error_already_set();
        }
        assert(isinstance<iterable>(items));
        return convert_elements(dict(reinterpret_borrow<iterable>(items)), convert);
    }

    template <typename T>
    static handle cast(T &&src, const return_value_policy_pack &rvpp, handle parent) {
        dict d;
        return_value_policy_pack rvpp_key = rvpp.get(0);
        return_value_policy_pack rvpp_value = rvpp.get(1);
        if (!std::is_lvalue_reference<T>::value) {
            rvpp_key = rvpp_key.override_policy(return_value_policy_override<Key>::policy);
            rvpp_value = rvpp_value.override_policy(return_value_policy_override<Value>::policy);
        }
        for (auto &&kv : src) {
            auto key = reinterpret_steal<object>(
                key_conv::cast(detail::forward_like<T>(kv.first), rvpp_key, parent));
            auto value = reinterpret_steal<object>(
                value_conv::cast(detail::forward_like<T>(kv.second), rvpp_value, parent));
            if (!key || !value) {
                return handle();
            }
            d[std::move(key)] = std::move(value);
        }
        return d.release();
    }

    PYBIND11_TYPE_CASTER_RVPP(Type,
                              const_name("Dict[") + key_conv::name + const_name(", ")
                                  + value_conv::name + const_name("]"));
};

template <typename Type, typename Value>
struct list_caster {
    using value_conv = make_caster<Value>;

    bool load(handle src, bool convert) {
        if (!PyObjectTypeIsConvertibleToStdVector(src.ptr())) {
            return false;
        }
        if (isinstance<sequence>(src)) {
            return convert_elements(src, convert);
        }
        if (!convert) {
            return false;
        }
        // Designed to be behavior-equivalent to passing tuple(src) from Python:
        // The conversion to a tuple will first exhaust the generator object, to ensure that
        // the generator is not left in an unpredictable (to the caller) partially-consumed
        // state.
        assert(isinstance<iterable>(src));
        return convert_elements(tuple(reinterpret_borrow<iterable>(src)), convert);
    }

private:
    template <typename T = Type, enable_if_t<has_reserve_method<T>::value, int> = 0>
    void reserve_maybe(const sequence &s, Type *) {
        value.reserve(s.size());
    }
    void reserve_maybe(const sequence &, void *) {}

    bool convert_elements(handle seq, bool convert) {
        auto s = reinterpret_borrow<sequence>(seq);
        value.clear();
        reserve_maybe(s, &value);
        for (auto it : seq) {
            value_conv conv;
            if (!conv.load(it, convert)) {
                return false;
            }
            value.push_back(cast_op<Value &&>(std::move(conv)));
        }
        return true;
    }

public:
    template <typename T>
    static handle cast(T &&src, const return_value_policy_pack &rvpp, handle parent) {
        return_value_policy_pack rvpp_local = rvpp.get(0);
        if (!std::is_lvalue_reference<T>::value) {
            rvpp_local = rvpp_local.override_policy(return_value_policy_override<Value>::policy);
        }
        list l(src.size());
        ssize_t index = 0;
        for (auto &&value : src) {
            auto value_ = reinterpret_steal<object>(
                value_conv::cast(detail::forward_like<T>(value), rvpp_local, parent));
            if (!value_) {
                return handle();
            }
            PyList_SET_ITEM(l.ptr(), index++, value_.release().ptr()); // steals a reference
        }
        return l.release();
    }

    PYBIND11_TYPE_CASTER_RVPP(Type, const_name("List[") + value_conv::name + const_name("]"));
};

template <typename Type, typename Alloc>
struct type_caster<std::vector<Type, Alloc>> : list_caster<std::vector<Type, Alloc>, Type> {};

template <typename Type, typename Alloc>
struct type_caster<std::deque<Type, Alloc>> : list_caster<std::deque<Type, Alloc>, Type> {};

template <typename Type, typename Alloc>
struct type_caster<std::list<Type, Alloc>> : list_caster<std::list<Type, Alloc>, Type> {};

template <typename ArrayType, typename Value, bool Resizable, size_t Size = 0>
struct array_caster {
    using value_conv = make_caster<Value>;

private:
    template <bool R = Resizable>
    bool require_size(enable_if_t<R, size_t> size) {
        if (value.size() != size) {
            value.resize(size);
        }
        return true;
    }
    template <bool R = Resizable>
    bool require_size(enable_if_t<!R, size_t> size) {
        return size == Size;
    }

    bool convert_elements(handle seq, bool convert) {
        auto l = reinterpret_borrow<sequence>(seq);
        if (!require_size(l.size())) {
            return false;
        }
        size_t ctr = 0;
        for (auto it : l) {
            value_conv conv;
            if (!conv.load(it, convert)) {
                return false;
            }
            value[ctr++] = cast_op<Value &&>(std::move(conv));
        }
        return true;
    }

public:
    bool load(handle src, bool convert) {
        if (!PyObjectTypeIsConvertibleToStdVector(src.ptr())) {
            return false;
        }
        if (isinstance<sequence>(src)) {
            return convert_elements(src, convert);
        }
        if (!convert) {
            return false;
        }
        // Designed to be behavior-equivalent to passing tuple(src) from Python:
        // The conversion to a tuple will first exhaust the generator object, to ensure that
        // the generator is not left in an unpredictable (to the caller) partially-consumed
        // state.
        assert(isinstance<iterable>(src));
        return convert_elements(tuple(reinterpret_borrow<iterable>(src)), convert);
    }

    template <typename T>
    static handle cast(T &&src, const return_value_policy_pack &rvpp, handle parent) {
        return_value_policy_pack rvpp_local = rvpp.get(0);
        list l(src.size());
        ssize_t index = 0;
        for (auto &&value : src) {
            auto value_ = reinterpret_steal<object>(
                value_conv::cast(detail::forward_like<T>(value), rvpp_local, parent));
            if (!value_) {
                return handle();
            }
            PyList_SET_ITEM(l.ptr(), index++, value_.release().ptr()); // steals a reference
        }
        return l.release();
    }

    PYBIND11_TYPE_CASTER_RVPP(ArrayType,
                              const_name<Resizable>(const_name(""), const_name("Annotated["))
                                  + const_name("List[") + value_conv::name + const_name("]")
                                  + const_name<Resizable>(const_name(""),
                                                          const_name(", FixedSize(")
                                                              + const_name<Size>()
                                                              + const_name(")]")));
};

template <typename Type, size_t Size>
struct type_caster<std::array<Type, Size>>
    : array_caster<std::array<Type, Size>, Type, false, Size> {};

template <typename Type>
struct type_caster<std::valarray<Type>> : array_caster<std::valarray<Type>, Type, true> {};

template <typename Key, typename Compare, typename Alloc>
struct type_caster<std::set<Key, Compare, Alloc>>
    : set_caster<std::set<Key, Compare, Alloc>, Key> {};

template <typename Key, typename Hash, typename Equal, typename Alloc>
struct type_caster<std::unordered_set<Key, Hash, Equal, Alloc>>
    : set_caster<std::unordered_set<Key, Hash, Equal, Alloc>, Key> {};

template <typename Key, typename Value, typename Compare, typename Alloc>
struct type_caster<std::map<Key, Value, Compare, Alloc>>
    : map_caster<std::map<Key, Value, Compare, Alloc>, Key, Value> {};

template <typename Key, typename Value, typename Hash, typename Equal, typename Alloc>
struct type_caster<std::unordered_map<Key, Value, Hash, Equal, Alloc>>
    : map_caster<std::unordered_map<Key, Value, Hash, Equal, Alloc>, Key, Value> {};

// This type caster is intended to be used for std::optional and std::experimental::optional
template <typename Type, typename Value = typename Type::value_type>
struct optional_caster {
    using value_conv = make_caster<Value>;

    template <typename T>
    static handle cast(T &&src, const return_value_policy_pack &rvpp, handle parent) {
        if (!src) {
            return none().release();
        }
        return_value_policy_pack rvpp_local = rvpp.get(0);
        if (!std::is_lvalue_reference<T>::value) {
            rvpp_local = rvpp_local.override_policy(return_value_policy_override<Value>::policy);
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return value_conv::cast(*std::forward<T>(src), rvpp_local, parent);
    }

    bool load(handle src, bool convert) {
        if (!src) {
            return false;
        }
        if (src.is_none()) {
            return true; // default-constructed value is already empty
        }
        value_conv inner_caster;
        if (!inner_caster.load(src, convert)) {
            return false;
        }

        value.emplace(cast_op<Value &&>(std::move(inner_caster)));
        return true;
    }

    PYBIND11_TYPE_CASTER_RVPP(Type, const_name("Optional[") + value_conv::name + const_name("]"));
};

#if defined(PYBIND11_HAS_OPTIONAL)
template <typename T>
struct type_caster<std::optional<T>> : public optional_caster<std::optional<T>> {};

template <>
struct type_caster<std::nullopt_t> : public void_caster<std::nullopt_t> {};
#endif

#if defined(PYBIND11_HAS_EXP_OPTIONAL)
template <typename T>
struct type_caster<std::experimental::optional<T>>
    : public optional_caster<std::experimental::optional<T>> {};

template <>
struct type_caster<std::experimental::nullopt_t>
    : public void_caster<std::experimental::nullopt_t> {};
#endif

/// Visit a variant and cast any found type to Python
struct variant_caster_visitor {
    return_value_policy_pack rvpp;
    handle parent;

    using result_type = handle; // required by boost::variant in C++11

    template <typename T>
    result_type operator()(T &&src) const {
        return make_caster<T>::cast(std::forward<T>(src), rvpp, parent);
    }
};

/// Helper class which abstracts away variant's `visit` function. `std::variant` and similar
/// `namespace::variant` types which provide a `namespace::visit()` function are handled here
/// automatically using argument-dependent lookup. Users can provide specializations for other
/// variant-like classes, e.g. `boost::variant` and `boost::apply_visitor`.
template <template <typename...> class Variant>
struct visit_helper {
    template <typename... Args>
    static auto call(Args &&...args) -> decltype(visit(std::forward<Args>(args)...)) {
        return visit(std::forward<Args>(args)...);
    }
};

/// Generic variant caster
template <typename Variant>
struct variant_caster;

template <template <typename...> class V, typename... Ts>
struct variant_caster<V<Ts...>> {
    static_assert(sizeof...(Ts) > 0, "Variant must consist of at least one alternative.");

    template <typename U, typename... Us>
    bool load_alternative(handle src, bool convert, type_list<U, Us...>) {
        PYBIND11_WARNING_PUSH
        PYBIND11_WARNING_DISABLE_GCC("-Wmaybe-uninitialized")
        auto caster = make_caster<U>();
        if (caster.load(src, convert)) {
            value = cast_op<U>(std::move(caster));
            return true;
        }
        return load_alternative(src, convert, type_list<Us...>{});
        PYBIND11_WARNING_POP
    }

    bool load_alternative(handle, bool, type_list<>) { return false; }

    bool load(handle src, bool convert) {
        // Do a first pass without conversions to improve constructor resolution.
        // E.g. `py::int_(1).cast<variant<double, int>>()` needs to fill the `int`
        // slot of the variant. Without two-pass loading `double` would be filled
        // because it appears first and a conversion is possible.
        if (convert && load_alternative(src, false, type_list<Ts...>{})) {
            return true;
        }
        return load_alternative(src, convert, type_list<Ts...>{});
    }

    template <typename Variant>
    static handle cast(Variant &&src, const return_value_policy_pack &rvpp, handle parent) {
        return visit_helper<V>::call(variant_caster_visitor{rvpp, parent},
                                     std::forward<Variant>(src));
    }

    using Type = V<Ts...>;
    PYBIND11_TYPE_CASTER_RVPP(Type,
                              const_name("Union[") + detail::concat(make_caster<Ts>::name...)
                                  + const_name("]"));
};

#if defined(PYBIND11_HAS_VARIANT)
template <typename... Ts>
struct type_caster<std::variant<Ts...>> : variant_caster<std::variant<Ts...>> {};

template <>
struct type_caster<std::monostate> : public void_caster<std::monostate> {};
#endif

PYBIND11_NAMESPACE_END(detail)

inline std::ostream &operator<<(std::ostream &os, const handle &obj) {
#ifdef PYBIND11_HAS_STRING_VIEW
    os << str(obj).cast<std::string_view>();
#else
    os << (std::string) str(obj);
#endif
    return os;
}

PYBIND11_NAMESPACE_END(PYBIND11_NAMESPACE)

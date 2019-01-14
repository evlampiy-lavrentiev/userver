#pragma once

#include <new>
#include <type_traits>
#include <utility>

namespace utils {

/**
 * FastPimpl implements fast pimpl idiom. Pimpl required an additional memory
 * allocation for creation and a pointer dereference for each data access.
 * FastPimpl doesn't require either memory allocation or indirect memory access.
 * But you have to manually set object size when you instantiate FastPimpl.
 */
template <class T, size_t Size, size_t Alignment, bool Strict = false>
class FastPimpl {
 public:
  FastPimpl(FastPimpl&& v) noexcept(noexcept(T(std::declval<T>())))
      : FastPimpl(std::move(*v)) {}

  FastPimpl(const FastPimpl& v) noexcept(noexcept(T(std::declval<const T&>())))
      : FastPimpl(*v) {}

  FastPimpl& operator=(const FastPimpl& rhs) noexcept(
      noexcept(std::declval<T&>() = std::declval<const T&>())) {
    *AsHeld() = *rhs;
    return *this;
  }

  FastPimpl& operator=(FastPimpl&& rhs) noexcept(
      noexcept(std::declval<T&>() = std::declval<T>())) {
    *AsHeld() = std::move(*rhs);
    return *this;
  }

  template <class... Args>
  explicit FastPimpl(Args&&... args) noexcept(
      noexcept(T(std::declval<Args>()...))) {
    new (AsHeld()) T(std::forward<Args>(args)...);
  }

  T* operator->() noexcept { return AsHeld(); }

  const T* operator->() const noexcept { return AsHeld(); }

  T& operator*() noexcept { return *AsHeld(); }

  const T& operator*() const noexcept { return *AsHeld(); }

  ~FastPimpl() noexcept {
    validator<sizeof(T), alignof(T)>::validate();
    AsHeld()->~T();
  }

 private:
  // Separate class for better diagnostics: with it actual sizes are visible in
  // compiler error message.
  template <std::size_t ActualSize, std::size_t ActualAlignment>
  struct validator {
    static void validate() noexcept {
      static_assert(
          Size >= ActualSize,
          "incorrect specialization of Size: Size is less than sizeof(T)");
      static_assert(
          Size == ActualSize || !Strict,
          "incorrect specialization of Size: Size and sizeof(T) mismatch");
      static_assert(Alignment % ActualAlignment == 0,
                    "incorrect specialization of Alignment: Alignment and "
                    "alignment_of(T) mismatch");
    }
  };

  std::aligned_storage_t<Size, Alignment> storage_;

  T* AsHeld() noexcept { return reinterpret_cast<T*>(&storage_); }

  const T* AsHeld() const noexcept {
    return reinterpret_cast<const T*>(&storage_);
  }
};

}  // namespace utils

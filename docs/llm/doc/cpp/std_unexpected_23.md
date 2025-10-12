Here is a full tutorial-style explanation of using `std::unexpected` in C++23, with pros and cons, tailored for someone familiar with `std::optional` and exceptions.

***

## Introduction to `std::unexpected`

`std::unexpected` is a new utility in C++23 designed to work with `std::expected`, a type that holds either a value (success) or an error (failure). It helps explicitly represent and propagate errors in functions without using exceptions.

`std::expected<T, E>` stores the expected type `T` or an error type `E`. `std::unexpected<E>` specifically wraps an error value of type `E` and signals an error state.

***

## Basic Usage

### 1. Include the header

```cpp
#include <expected>
```

### 2. Function returning `std::expected`

A function returns either a successful value or an error wrapped in `std::unexpected`:

```cpp
#include <string>
#include <expected>
#include <iostream>

std::expected<int, std::string> stringToInt(const std::string &input) {
    int value{};
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (ec == std::errc()) {
        return value; // Success: returns int value
    }
    if (ec == std::errc::invalid_argument) {
        return std::unexpected("Invalid number format"); // Error: with unexpected
    }
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected("Number out of range"); // Error: with unexpected
    }
    return std::unexpected("Unknown error");
}

int main() {
    auto result = stringToInt("123abc");
    if (result) {
        std::cout << "Parsed number: " << *result << '\n';
    } else {
        std::cout << "Error: " << result.error() << '\n';
    }
}
```

Here `std::unexpected` wraps the error string, signaling failure. Consumers check if the expected value exists with `if (result)`, else get the error with `result.error()`.

***

## Key Interface and Member Functions

- `expected.has_value()` or `operator bool()` — true if valid result exists.
- `expected.value()` — access the value, throws `std::bad_expected_access` if no value.
- `expected.error()` — access the error wrapped by `std::unexpected`.
- Use `std::unexpected` construction for error return: `return std::unexpected(error_value);`

***

## Comparison with `std::optional` and Exceptions

| Feature                  | `std::optional`                           | `std::expected` + `std::unexpected`        | Exceptions                           |
|--------------------------|------------------------------------------|---------------------------------------------|-------------------------------------|
| Return nothing or value   | Yes                                      | No (returns value or error)                  | No (throws on error)                 |
| Contain error info       | No                                       | Yes — can store detailed error information  | Yes — via exception objects          |
| Error handling style     | Explicit check for value presence        | Explicit check for value or error            | Implicit try-catch error handling    |
| Performance             | Minimal overhead                          | Minimal overhead, no stack unwinding         | Can be heavy due to stack unwinding  |
| Programmer control      | Checks missing value                      | Checks both success & specific error         | Control lost to runtime exceptions   |

***

## Advantages of `std::unexpected` in C++23

- **Explicit error signaling:** Error case is explicit and contained within the type system, unlike exceptions.
- **Lightweight handling:** No cost of stack unwinding like exceptions.
- **Rich error info:** Errors can be any type — string message, enum, or error class.
- **Monadic operations:** Works well with `std::expected` monadic helpers like `and_then`, `or_else`.

***

## Disadvantages and Considerations

- **More verbose:** Requires explicit error checking everywhere, unlike exceptions.
- **Error propagation:** Requires manual forwarding of errors, unlike implicit with exceptions.
- **Learning curve:** New API for many developers who are used to exceptions.
- **Not a drop-in replacement:** Some legacy codebases rely on exceptions, so migration requires redesign.

***

## Summary

`std::unexpected` in C++23 is best used when you want explicit, lightweight, and rich error handling without exceptions. It pairs with `std::expected` to clearly separate success and error cases, improving error clarity and performance in many scenarios.

This approach aligns with modern functional programming error patterns and makes error states first-class, typed values.[1][2][4][6][8]

***

Citations:
[1] [C++23: A New Way of Error Handling with std::expected](https://www.modernescpp.com/index.php/c23-a-new-way-of-error-handling-with-stdexpected/)  
[2] [C++23: The <expected> header; expect the unexpected](https://www.sandordargo.com/blog/2022/11/16/cpp23-expected)  
[3] [c++ - How to convert a std::optional to a std::expected?](https://stackoverflow.com/questions/79491938/how-to-convert-a-stdoptional-to-a-stdexpected)  
[4] [Using std::expected from C++23](https://www.cppstories.com/2024/expected-cpp23/)  
[5] [Functional exception-less error handling with C++23's ...](https://devblogs.microsoft.com/cppblog/cpp23s-optional-and-expected/)  
[6] [C++23 - std::expected, the superior way of returning a value or ...](https://andreasfertig.com/blog/2022/09/cpp23-stdexpected-the-superior-way-of-returning-a-value-or-an-error/)  
[7] [Functional exception-less error handling with C++23's ...](https://www.reddit.com/r/cpp/comments/12r0992/functional_exceptionless_error_handling_with_c23s/)  
[8] [std::unexpected](https://en.cppreference.com/w/cpp/utility/expected/unexpected.html)  
[9] [How to Use Monadic Operations for `std::optional` in C++23](https://www.cppstories.com/2023/monadic-optional-ops-cpp23/)  
[10] [std::optional](https://en.cppreference.com/w/cpp/utility/optional.html)
# cpp-modernize — evaluator notes

This task is exclusively about C++20 idiom adoption with behavior preserved.
Items to weight more heavily:

- **Modern C++20 idioms** — the entire purpose of the benchmark. Score every
  item in this category strictly. Specifically check that ALL of these are gone
  from the result:
  - raw `new` / `delete` and owning raw pointers → smart pointers / containers
  - output-parameter lookup → `std::optional<Value>`
  - unscoped `enum` → `enum class`
  - `NULL` → `nullptr`
  - `typedef` → `using`
  - `get_`-prefixed getters → field-named getters, `[[nodiscard]] ... const`
  - manual index loops → range-for / algorithms; verbose iterator types → `auto`
  - `CMAKE_CXX_STANDARD` bumped to 20
  A residual of any one of these is a miss on the corresponding rubric item.
- **Memory and resource safety** — the `new`/`delete` → `unique_ptr` conversion
  is the highest-value change. After the refactor there must be no manual
  `delete` and no leak on any path. If the original had a destructor doing
  manual cleanup, the modern version should rely on RAII (rule-of-zero) instead.
- **Compilation and build** — regression-free is the floor. If the refactor
  breaks an existing test, score as a fundamental failure regardless of how
  clean the new idioms look.

Items that matter LESS for this task:
- Error handling semantics — the registry's error *conditions* don't change;
  only their *representation* (out-param → optional) does. Don't expect new
  error paths.
- Concurrency and determinism — single-threaded; N/A.
- Testing breadth — the existing cases stay; the agent updates them to the new
  signatures, not add new coverage.

Watch for: agents that "improve" beyond the task — adding features, new error
paths, or renaming public operations is out of scope. The task says preserve
behavior. Score down in Compilation and build if "improvements" break the suite.

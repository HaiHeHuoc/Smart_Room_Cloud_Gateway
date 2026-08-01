# C/C++ Source File Skeleton

Use this template as the standard starting structure for new `.c` or `.cpp`
source files and as the section-order reference when cleaning an existing file.

```c
/* Includes ----------------------------------------------------------------- */
/* Include standard libraries, ESP-IDF headers, and component headers here. */

/* Macros ------------------------------------------------------------------- */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants ---------------------------------------------------------------- */
/* Define file-scope const values here. */

/* Type Definitions --------------------------------------------------------- */
/* Define local enums, structs, unions, and typedefs here. */

/* Static Variables --------------------------------------------------------- */
/* Define file-scope static variables here. */

/* Global Variables --------------------------------------------------------- */
/* Define file-scope global variables here.
 *
 * Avoid global variables when possible.
 * Prefer static file-scope variables unless external linkage is required.
 */

/* Function Prototypes ------------------------------------------------------ */
/* Declare static helper functions here. */

/* Application -------------------------------------------------------------- */
/* Implement the main public application logic here.
 *
 * Examples:
 * - component initialization
 * - task creation
 * - event handling
 * - public module entry points
 */

/* Static Functions --------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions ---------------------------------------------------------------- */
/* Implement non-static/public functions here. */
```

## Public Header Skeleton

Use the following reduced structure for public `.h` files. Omit empty categories
rather than adding placeholder sections with no content.

```c
#pragma once

/**
 * @file component_name.h
 * @brief One-line public module purpose.
 *
 * Describe ownership, execution context, lifetime, and important exclusions.
 */

/* Includes ----------------------------------------------------------------- */

#ifdef __cplusplus
extern "C"
{
#endif

/* Macros ------------------------------------------------------------------- */
/* Public macros only when required. */

/* Constants ---------------------------------------------------------------- */
/* Public constants only when required. */

/* Type Definitions --------------------------------------------------------- */
/* Public enums, structs, and callback types. */

/* Functions ---------------------------------------------------------------- */
/* Public API declarations with Doxygen contracts. */

#ifdef __cplusplus
}
#endif
```

## Codex Usage Instruction

When creating a new C or C++ source/header file, use the corresponding
structure above as the default layout.

Rules:

1. Keep each definition in its corresponding section.
2. Add or remove sections only when necessary.
3. Omit an empty section instead of leaving unused placeholders.
4. Prefer `static` for variables and helper functions used only inside a source
   file.
5. Keep public functions in the `Functions` section.
6. Keep internal helper functions in the `Static Functions` section.
7. Do not leave unused placeholder variables, functions, macros, or includes.
8. Preserve the section order unless the file has a strong documented reason
   to use another structure.
9. Add comments only where they explain intent, constraints, ownership,
   concurrency, hardware behavior, cleanup, or non-obvious logic.
10. Public API docstrings should state relevant parameters, return values,
    prerequisites, blocking behavior, task/ISR/callback context, ownership,
    lifetime, timeouts, and security constraints.

## Applying the Skeleton to Existing Files

For an existing implementation, the skeleton is a documentation and layout
reference, not authorization for a rewrite.

- Preserve executable statements, declaration values, API signatures, control
  flow, behavior, and build configuration unless the task explicitly permits
  code changes.
- Do not reorder functions merely to make the file resemble the template when
  doing so would create a large or risky diff.
- It is acceptable to normalize section labels, add a missing module docstring,
  improve Doxygen contracts, and remove stale comments without touching code.
- Do not add empty `Application` or `Global Variables` sections to an existing
  file that has no content for them.
- For a documentation-only phase closure, verify that every changed line is
  Markdown, a comment/docstring, or a section label.
- If safe skeleton application would require moving or rewriting executable
  code, leave the file unchanged and document the deferred cleanup.

## Phase 7.6 Application

Phase 7.6 applies this skeleton in documentation-only mode. Touched public
headers use the reduced header structure and refreshed module/API docstrings.
No executable source statement, API declaration, configuration value, timing,
or runtime behavior is changed by that closure.

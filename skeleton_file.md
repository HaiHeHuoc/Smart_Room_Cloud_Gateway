# C/C++ Source File Skeleton

Use this template as a standard starting structure for new `.c` or `.cpp` source files.

```c
/* Includes ----------------------------------------------------------------- */
/* Include standard libraries, ESP-IDF headers, component headers, etc. here. */

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

## Codex Usage Instruction

When creating a new C or C++ source file, use the structure above as the default layout.

Rules:

1. Keep each definition in its corresponding section.
2. Add or remove sections only when necessary.
3. Prefer `static` for variables and helper functions used only inside the file.
4. Keep public functions in the `Functions` section.
5. Keep internal helper functions in the `Static Functions` section.
6. Do not leave unused placeholder variables, functions, or includes.
7. Preserve the section order unless the file has a strong reason to use another structure.
8. Add short comments only where they explain intent, constraints, hardware behavior, or non-obvious logic.

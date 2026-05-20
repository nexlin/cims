---
name: feedback_code_style
description: Code style and approach preferences observed in this project
type: feedback
---

Keep Python files validated with `ast.parse()` after edits to catch syntax errors early.

**Why:** The csc_service.py and related files are large and complex; syntax errors break the entire server.
**How to apply:** After any Python edit, run `python3 -c "import ast; ast.parse(open('file').read())"` to verify.

Routing conflicts: when two URL patterns share a prefix, use a single handler that dispatches on path depth rather than registering two separate routes.

**Why:** The HttpServer may match the more specific route first or cause conflicts.
**How to apply:** Check for prefix overlaps in handler lists; consolidate into one handler with internal dispatch.

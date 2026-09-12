# Isolated Target Test Sources

This directory holds hardware-only test material that must not be added to the
production Gateway `main` component. Each test application or helper owns only
the resources documented in its local README and requires an explicit,
separate target test build or profile.

Production audio ownership remains exclusively with `audio_manager`.

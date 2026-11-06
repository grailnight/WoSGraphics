"""Guard: the former generator silently removed installed renderer features."""
raise SystemExit(
    'Unsafe legacy integration regeneration is blocked. '
    'The current authoritative integration was recovered and its executable '
    'sections match the installed ac2a629d renderer. Build the existing '
    'integration with build_integration.ps1. See ../effects_current/README_RU.md '
    'and the verified source checkpoint. The legacy generator is archived '
    'at ../effects_current/prepare_integration.legacy_unsafe.py for research only.'
)

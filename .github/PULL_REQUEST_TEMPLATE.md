## Summary

<!-- Explain what changed and why. Keep this focused on one logical change. -->

## Change Type

- [ ] Bug fix
- [ ] Feature
- [ ] Refactor
- [ ] Documentation
- [ ] Build / CI / tooling
- [ ] Security hardening

## Affected Areas

<!-- Components, files, GPIOs, tasks, queues, APIs, storage schema, UI screens. -->

## Validation

- [ ] `idf.py build` passed
- [ ] Flash/boot test passed
- [ ] Target-hardware behavior verified
- [ ] Existing behavior regression-tested
- [ ] Documentation updated
- [ ] Validation was not applicable; explanation is included below

### Test Environment

```text
Board:
ESP-IDF version:
Commit:
Power source:
Connected peripherals:
Test duration:
```

### Evidence

<!-- Add sanitized logs, screenshots, or video links. Never expose credentials. -->

## Embedded Impact Review

- [ ] No unsafe ISR API usage was introduced
- [ ] LVGL remains owned by the UI task
- [ ] Blocking work is not performed from producer callbacks
- [ ] Stack and heap impact was considered
- [ ] DMA/internal-RAM requirements were considered
- [ ] Timeouts and retry paths remain bounded
- [ ] Credential and sensitive-buffer lifetime was reviewed

## Security Checklist

- [ ] No password, token, private key, Wi-Fi credential, or service-account file
      is included
- [ ] No local `sdkconfig`, `.env`, firmware binary, NVS dump, or private log is
      included
- [ ] Media contains no readable provisioning QR/PoP, account data, or private
      notifications
- [ ] New external actions/dependencies are pinned and justified
- [ ] Repository workflows use minimum required permissions

## Risk And Rollback

<!-- What could break? How can this change be reverted safely? -->

## Remaining Limitations

<!-- State anything not tested, deferred, or deliberately unsupported. -->

## Maintainer Review

- [ ] CODEOWNERS review completed
- [ ] Required checks passed
- [ ] Review conversations resolved
- [ ] Safe to merge into protected `main`

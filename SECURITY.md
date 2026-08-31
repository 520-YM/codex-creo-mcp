# Security

## Repository scope

This repository contains source code and sanitized templates only. It must not contain company CAD data, real Creo configuration files, credentials, proprietary third-party binaries, diagnostic trails, screenshots, or recordings.

## File-system boundary

Formal tools obtain the working directory from the live Creo session. They reject caller-supplied directories, `project_name`, path traversal, and nested model paths.

## Model writes

Write tools should validate the expected model, feature owner, dimension symbol, expected old value, and relation-driven state before mutation. Results must be regenerated and read back before save. Cleanup targets only verified model families and uses the Windows Recycle Bin.

## Reporting

Do not publish a security issue containing proprietary CAD files or production paths. Provide a minimal synthetic model and redacted logs.

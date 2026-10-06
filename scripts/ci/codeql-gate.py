#!/usr/bin/env python3
"""Require a successful exact-head CodeQL run and readable, ref-scoped results."""

import json
import os
import re
import subprocess
import sys
import time


class GateError(RuntimeError):
    pass


def api(repository, endpoint, **params):
    command = ["gh", "api", f"repos/{repository}/{endpoint}", "--method", "GET"]
    for key, value in params.items():
        command.extend(["-f", f"{key}={value}"])
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise GateError(f"GitHub API failed for {endpoint}: {result.stderr.strip()}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise GateError(f"Invalid GitHub API JSON for {endpoint}") from error


def pages(repository, endpoint, **params):
    # Explicit pages make malformed responses fail rather than disappear inside jq.
    for page in range(1, 10001):
        records = api(repository, endpoint, per_page=100, page=page, **params)
        if not isinstance(records, list) or any(not isinstance(row, dict) for row in records):
            raise GateError(f"Invalid GitHub API records for {endpoint}")
        yield from records
        if len(records) < 100:
            return
    raise GateError(f"GitHub API pagination limit exceeded for {endpoint}")


def run_ref(run):
    if run.get("event") == "pull_request":
        pulls = run.get("pull_requests")
        if not isinstance(pulls, list) or len(pulls) != 1:
            raise GateError("CodeQL run has no unambiguous pull request ref")
        number = pulls[0].get("number")
        if not isinstance(number, int) or number <= 0:
            raise GateError("Invalid CodeQL pull request number")
        return f"refs/pull/{number}/merge"
    branch = run.get("head_branch")
    if not isinstance(branch, str) or not branch:
        raise GateError("CodeQL run has no branch ref")
    return f"refs/heads/{branch}"


def successful_run(repository, sha):
    for attempt in range(1, 91):
        response = api(repository, "actions/workflows/codeql.yml/runs", head_sha=sha, per_page=1)
        runs = response.get("workflow_runs") if isinstance(response, dict) else None
        if not isinstance(runs, list) or len(runs) > 1:
            raise GateError("Invalid CodeQL workflow-run response")
        if not runs:
            print(f"{attempt}/90: no CodeQL run for {sha}", flush=True)
        else:
            run = runs[0]
            if not isinstance(run, dict) or run.get("head_sha") != sha:
                raise GateError("CodeQL workflow run does not match the requested head")
            status = run.get("status")
            if status == "completed":
                if run.get("conclusion") != "success":
                    raise GateError(f"CodeQL concluded {run.get('conclusion')}")
                return run
            if status not in ("queued", "in_progress", "waiting", "pending", "requested"):
                raise GateError(f"Unknown CodeQL run status: {status}")
            print(f"{attempt}/90: CodeQL {status}", flush=True)
        time.sleep(30)
    raise GateError("CodeQL timeout: no successful exact-head run")


def check_analysis(repository, sha, ref):
    analyses = pages(repository, "code-scanning/analyses", ref=ref, tool_name="CodeQL")
    # The API defaults to newest first. Never accept an older matching analysis
    # when this ref's newest C/C++ analysis belongs to a different commit.
    for analysis in analyses:
        if analysis.get("category") != "/language:c-cpp":
            continue
        if analysis.get("ref") != ref or analysis.get("commit_sha") != sha:
            raise GateError("Newest C/C++ CodeQL analysis does not match the exact head/ref")
        if analysis.get("error") != "" or analysis.get("tool", {}).get("name") != "CodeQL":
            raise GateError("CodeQL analysis is missing or reports an error")
        return
    raise GateError("No uploaded C/C++ CodeQL analysis for the analyzed ref")


def check_alerts(repository, ref):
    alerts = list(pages(repository, "code-scanning/alerts", state="open", ref=ref))
    if alerts:
        raise GateError(f"{len(alerts)} open code scanning alert(s) on {ref}")


def main():
    repository = os.environ["GITHUB_REPOSITORY"]
    sha = os.environ["CODEQL_HEAD_SHA"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise GateError("Invalid CodeQL head SHA")
    run = successful_run(repository, sha)
    ref = run_ref(run)
    print(f"CodeQL successful run={run.get('id')} head={sha} ref={ref}", flush=True)
    time.sleep(60)
    check_analysis(repository, sha, ref)
    check_alerts(repository, ref)
    time.sleep(15)
    # A moving branch must not turn an older clean upload into current evidence.
    check_analysis(repository, sha, ref)
    check_alerts(repository, ref)
    print(f"CodeQL gate passed for {sha} on {ref}: zero open alerts", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (GateError, KeyError, OSError) as error:
        print(f"BLOCKED: {error}", file=sys.stderr)
        sys.exit(1)

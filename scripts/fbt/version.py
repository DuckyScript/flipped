import datetime
import subprocess
from functools import cache


@cache
def get_git_commit_unix_timestamp():
    try:
        return int(subprocess.check_output(["git", "show", "-s", "--format=%ct"]))
    except Exception:
        return int(datetime.datetime.now().timestamp())


@cache
def get_fast_git_version_id():
    try:
        version = (
            subprocess.check_output(
                [
                    "git",
                    "describe",
                    "--always",
                    "--dirty",
                    "--all",
                    "--long",
                ]
            )
            .strip()
            .decode()
        )
        return (version, datetime.date.today())
    except Exception as e:
        print("Failed to check for git changes", e)

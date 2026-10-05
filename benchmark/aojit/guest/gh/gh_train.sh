# gh (GitHub CLI, a static Go binary) recording workload: no account and no network needed.
# Every gh runs as a child of sh (on the Mac CLI a multi-threaded pid 1 can leave ish running after it
# exits). GH_CONFIG_DIR keeps the rootfs's own gh config untouched. The gh.holdout case's commands
# (help environment/formatting, issue, release) are left out on purpose.
export GH_CONFIG_DIR=/tmp/aojit/gh/cfg_train
rm -rf "$GH_CONFIG_DIR"
for i in 1 2 3; do gh --version > /dev/null; gh help > /dev/null; done
for s in bash zsh fish powershell; do gh completion -s $s > /dev/null; done
for c in auth repo pr workflow run gist api search codespace; do gh $c --help > /dev/null; done
gh pr create --help > /dev/null; gh repo clone --help > /dev/null
for k in editor pager prompt git_protocol; do gh config get $k > /dev/null; done
gh config set editor vi && gh config set git_protocol ssh && gh config list > /dev/null
gh alias set prc 'pr checkout' > /dev/null && gh alias list > /dev/null && gh alias delete prc > /dev/null
gh api user > /dev/null 2>&1 || true
gh repo view cli/cli > /dev/null 2>&1 || true
rm -rf "$GH_CONFIG_DIR"

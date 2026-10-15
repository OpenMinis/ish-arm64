# Candidate workload (profile.sh): agent-style git over the repo of git_setup.sh (run it once first).
# Use a fakefs rootfs (-f): on the Mac CLI's realfs, git add hangs.
cd /tmp/g/repo
for i in 1 2 3; do git status --short > /dev/null; git status > /dev/null; done
for f in $(find . -name '*.py' | head -60); do echo "# touched $RANDOM" >> $f; done
git diff --stat > /dev/null; git diff > /dev/null
git add -A && git commit -qm "agent: touch 60 files" && git log --oneline -20 > /dev/null
git log -p -3 > /dev/null; git show --stat HEAD > /dev/null; git blame json/decoder.py > /dev/null
git push -q origin HEAD:main
git gc -q --auto
rm -rf /tmp/g/clone && git clone -q --depth 1 https://github.com/octocat/Hello-World.git /tmp/g/clone || echo GITFAIL

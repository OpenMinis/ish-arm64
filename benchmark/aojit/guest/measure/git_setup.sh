# Setup for git.sh (not timed): a repo of about 2k files (three copies of the python stdlib, 15
# commits) and a local bare remote. Needs a fakefs rootfs (see git.sh).
git config --global --add safe.directory '*'; git config --global user.email a@b; git config --global user.name agent; git config --global init.defaultBranch main
rm -rf /tmp/g && mkdir -p /tmp/g && cd /tmp/g && git init -q repo && cd repo
for c in a b c; do mkdir $c; tar -C /usr/lib/python3.12 --exclude=site-packages --exclude=__pycache__ -cf - . | tar -xf - -C $c; done
set -- $(ls | head -300); n=0
for d in $(ls -d */*); do git add -A "$d"; n=$((n + 1)); [ $((n % 40)) -eq 0 ] && git commit -qm "import batch $n"; done
git add -A; git commit -qm "import rest"
git init -q --bare /tmp/g/remote.git && git remote add origin /tmp/g/remote.git && git push -q origin HEAD:main
git ls-files | wc -l; git log --oneline | wc -l

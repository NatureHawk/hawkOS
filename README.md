# hawkOS
Tiny hobby OS that boots with GRUB, sets up interrupts, and runs in QEMU.

## Build & run
```bash
sudo apt install -y build-essential gcc-multilib binutils qemu-system-x86 grub-pc-bin xorriso mtools make
make run

# Install GH CLI and log in (choose GitHub.com → SSH → device code)
sudo apt update
sudo apt install -y gh
gh auth login

# From your project
cd ~/Desktop/hawkOS

# This creates https://github.com/NatureHawk/hawkOS AND pushes your current code
gh repo create hawkOS --public --source=. --remote=origin --push

# Tag your stages and push tags
git tag -a v0.1-stage1 -m "Stage 1: boots + VGA text"
git tag -a v0.2-stage2 -m "Stage 2: IDT + PIT 100Hz timer"
git push --tags

# (Optional) Add CI to build your ISO on every push
mkdir -p .github/workflows
cat > .github/workflows/build.yml <<'EOF'
name: build-iso
on: [push]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update
      - run: sudo apt-get install -y build-essential gcc-multilib binutils grub-pc-bin xorriso mtools make
      - run: make iso
      - uses: actions/upload-artifact@v4
        with:
          name: hawkOS.iso
          path: hobby-os.iso

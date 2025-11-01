#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Syncing master to ultra (preserving ultra-specific files) ==="

# Check we're on master branch
CURRENT_BRANCH=$(git branch --show-current)
if [ "$CURRENT_BRANCH" != "master" ]; then
    echo "ERROR: Must be on master branch. Currently on: $CURRENT_BRANCH"
    exit 1
fi

# Check working tree is clean
if ! git diff-index --quiet HEAD --; then
    echo "ERROR: Working tree has uncommitted changes. Commit or stash them first."
    exit 1
fi

# List of patterns to EXCLUDE from sync (ultra-specific files)
EXCLUDE_PATTERNS=(
    # Sync script itself (master only)
    "^sync_master_to_ultra.sh$"
    
    # Build scripts (completely different)
    "^build.sh$"
    "^buildroot/board/luckfox-pico/"
    "^ext_tree/configs/"
    "^ext_tree/external.mk$"
    
    # Platform-specific kernel/boot configs
    "^ext_tree/board/luckfox/dts_max/"
    
    # Build hooks (different post-build.sh for MAX vs Ultra)
    "^ext_tree/board/luckfox/scripts/post-build.sh$"
    "^ext_tree/board/luckfox/scripts/post-image"
    "^ext_tree/board/luckfox/scripts/linux-post-build.sh$"
    
    # U-boot binaries (MAX only - pre-built)
    "^ext_tree/board/luckfox/uboot/"
    
    # Platform-specific rootfs files
    "^ext_tree/board/luckfox/rootfs_overlay/etc/fstab$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/fw_env.config$"
    
    # Platform-specific init scripts (MTD vs eMMC specific)
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S00platform$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S20linkmount$"
    "^ext_tree/board/luckfox/rootfs_overlay/etc/init.d/S94ioi2s$"
    "^ext_tree/board/luckfox/config/uboot-env.txt$"
    
    # Platform-specific helper scripts (DTB switching - different for MAX/Ultra)
    "^ext_tree/board/luckfox/rootfs_overlay/opt/2.*\.sh$"
    "^ext_tree/board/luckfox/rootfs_overlay/opt/export.sh$"
    "^ext_tree/board/luckfox/rootfs_overlay/opt/update.sh$"

    "^buildroot/output/"
)

echo "Step 1: Getting list of changed files in master..."
MASTER_HEAD=$(git rev-parse master)

# Stash current ultra state
echo "Step 2: Switching to ultra branch..."
git checkout ultra

ULTRA_HEAD=$(git rev-parse ultra)

echo "Master HEAD: $MASTER_HEAD"
echo "Ultra HEAD: $ULTRA_HEAD"

# Get list of files that differ between master and ultra (ignoring platform-specific)
echo "Step 3: Finding files that differ between master and ultra..."
CHANGED_FILES=$(git diff --name-only ultra master)

echo "Step 4: Filtering files (excluding ultra-specific)..."
FILES_TO_SYNC=""
SKIPPED_COUNT=0
SYNCED_COUNT=0

while IFS= read -r file; do
    if [ -z "$file" ]; then
        continue
    fi
    
    SKIP=false
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        if echo "$file" | grep -qE "$pattern"; then
            echo "  [SKIP] $file (matches: $pattern)"
            SKIP=true
            SKIPPED_COUNT=$((SKIPPED_COUNT + 1))
            break
        fi
    done
    
    if [ "$SKIP" = false ]; then
        FILES_TO_SYNC="$FILES_TO_SYNC $file"
        SYNCED_COUNT=$((SYNCED_COUNT + 1))
    fi
done <<< "$CHANGED_FILES"

echo ""
echo "Files to sync: $SYNCED_COUNT"
echo "Files skipped: $SKIPPED_COUNT"
echo ""

if [ -z "$FILES_TO_SYNC" ]; then
    echo "No files to sync!"
    exit 0
fi

echo "Step 5: Syncing files from master..."
for file in $FILES_TO_SYNC; do
    if git cat-file -e master:"$file" 2>/dev/null; then
        echo "  [SYNC] $file"
        # Create directory if needed
        mkdir -p "$(dirname "$file")"
        # Checkout file from master
        git checkout master -- "$file"
    else
        echo "  [DELETE] $file (removed in master)"
        git rm -f "$file" 2>/dev/null || rm -f "$file"
    fi
done

echo ""
echo "Step 6: Updating branding (MAX → Ultra)..."
INDEX_PHP="ext_tree/board/luckfox/rootfs_overlay/var/www/index.php"
if [ -f "$INDEX_PHP" ]; then
    if grep -q "MAX" "$INDEX_PHP"; then
        sed -i 's/MAX/Ultra/g' "$INDEX_PHP"
        git add "$INDEX_PHP"
        echo "  [UPDATED] $INDEX_PHP (MAX → Ultra)"
    fi
fi

echo ""
echo "Step 7: Cleaning up empty directories..."
# Find and remove empty directories (excluding .git and excluded paths)
find . -type d -empty -not -path "./.git/*" | while read -r dir; do
    # Remove leading ./ from path for pattern matching
    clean_dir="${dir#./}"
    
    SKIP=false
    for pattern in "${EXCLUDE_PATTERNS[@]}"; do
        if echo "$clean_dir" | grep -qE "$pattern"; then
            SKIP=true
            break
        fi
    done
    
    if [ "$SKIP" = false ]; then
        echo "  [RMDIR] $dir"
        rmdir "$dir" 2>/dev/null || true
    fi
done

echo ""
echo "Step 8: Reviewing changes..."
git status

echo ""
echo "=== Sync complete! ==="
echo ""
echo "Review the changes with: git diff"
echo "Commit with: git commit -m 'Sync from master'"
echo "Discard with: git reset --hard HEAD"

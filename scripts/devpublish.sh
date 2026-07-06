#!/usr/bin/env bash
# devpublish.sh — promote a devlog entry to dnuke.com blog
# Usage: scripts/devpublish.sh devlog/2026-03-25-123456-some-entry.md
#
# Adds Eleventy frontmatter (title, date, tags) and copies to dnuke.com/src/blog/posts/.
# Also copies any referenced screenshot from devlog/assets/ to dnuke.com/src/images/.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOML="$REPO_ROOT/.project.toml"

# Read publish config from .project.toml (simple grep, no toml parser needed)
DEVLOG_TAG=$(grep 'devlog_tag' "$TOML" | sed 's/.*= "\(.*\)"/\1/')
DEVLOG_DEST=$(grep 'devlog_dest' "$TOML" | sed 's/.*= "\(.*\)"/\1/')
DEST_DIR="$REPO_ROOT/$DEVLOG_DEST"

SOURCE="${1:-}"
if [[ -z "$SOURCE" ]]; then
  echo "Usage: $0 <devlog entry .md file>"
  echo ""
  echo "Recent entries:"
  ls -t "$REPO_ROOT/devlog/"*.md 2>/dev/null | head -10 | sed 's|.*/||'
  exit 1
fi

[[ "$SOURCE" != /* ]] && SOURCE="$REPO_ROOT/$SOURCE"

if [[ ! -f "$SOURCE" ]]; then
  echo "Error: $SOURCE not found"
  exit 1
fi

# Derive date and slug from filename (YYYY-MM-DD-HHmmss-slug.md)
BASENAME=$(basename "$SOURCE" .md)
DATE_PART=$(echo "$BASENAME" | grep -oP '^\d{4}-\d{2}-\d{2}')
SLUG_PART=$(echo "$BASENAME" | sed "s/^${DATE_PART}-[0-9]\{6\}-//")

DEST_FILENAME="${DATE_PART}_${SLUG_PART}.md"
DEST_FILE="$DEST_DIR/$DEST_FILENAME"

# Extract title from first H1 in the source
TITLE=$(grep -m1 '^# ' "$SOURCE" | sed 's/^# //')

# Read the source body (strip the first H1 since frontmatter will carry the title)
BODY=$(sed '1{/^# /d;}' "$SOURCE")

# Copy any screenshot referenced in the file to dnuke.com/src/images/
SCREENSHOT=$(grep -oP '(?<=\!\[\]\()assets/[^)]+(?=\))' "$SOURCE" || true)
if [[ -n "$SCREENSHOT" ]]; then
  SRC_IMG="$REPO_ROOT/devlog/$SCREENSHOT"
  IMG_BASENAME=$(basename "$SCREENSHOT")
  DEST_IMG="$REPO_ROOT/../dnuke.com/src/images/$IMG_BASENAME"
  if [[ -f "$SRC_IMG" ]]; then
    cp "$SRC_IMG" "$DEST_IMG"
    echo "   Copied image → src/images/$IMG_BASENAME"
    # Rewrite image path in body for dnuke.com
    BODY=$(echo "$BODY" | sed "s|assets/${IMG_BASENAME}|/images/${IMG_BASENAME}|g")
  fi
fi

# Write Eleventy post
{
  echo "---"
  echo "title: \"$TITLE\""
  echo "date: $DATE_PART"
  echo "tags:"
  echo "  - posts"
  echo "  - $DEVLOG_TAG"
  echo "---"
  echo ""
  echo "$BODY"
} > "$DEST_FILE"

echo "✅ Published: $DEST_FILE"
echo "   Title: $TITLE"
echo "   Date:  $DATE_PART"
echo "   Tags:  posts, $DEVLOG_TAG"
echo ""
echo "Next: cd ../dnuke.com && npm run serve"

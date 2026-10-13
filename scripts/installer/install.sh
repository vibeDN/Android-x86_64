#!/bin/sh
# Install Android-x86_64 to a target disk from the live system.
# POSIX sh, busybox-compatible. Run as root: adb shell su 0 /path/to/install.sh [--dry-run]
# Test mode: install.sh --source-image <img> --target-image <img> [--dry-run]
set -u
export PATH="$PATH:/usr/sbin:/sbin"

DRY_RUN=0
SOURCE_IMG=""
TARGET_IMG=""

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --source-image) SOURCE_IMG="$2"; shift ;;
    --target-image) TARGET_IMG="$2"; shift ;;
    *) echo "usage: $0 [--dry-run] [--source-image <img>] [--target-image <img>]" >&2; exit 1 ;;
  esac
  shift
done

ESP_START=2048
ESP_SIZE=$((1024*1024))

log() { printf '[install.sh] %s\n' "$*" >&2; }
run() {
  if [ "$DRY_RUN" = 1 ]; then
    log "DRY-RUN: $*"
  else
    log "RUN: $*"
    eval "$@" || die "Command failed: $*"
  fi
}
die() { log "ERROR: $*"; exit 1; }

detect_source_disk() {
  local boot_uuid disk
  boot_uuid=$(grep -o 'androidboot.boot_part_uuid=[^ ]*' /proc/cmdline 2>/dev/null | cut -d= -f2) || return 1
  for disk in /sys/block/sd*; do
    disk=$(basename "$disk")
    if sgdisk -i 9 "/dev/$disk" 2>/dev/null | grep -q "$boot_uuid"; then
      echo "/dev/$disk"
      log "Detected source disk: /dev/$disk (boot_a GUID: $boot_uuid)"
      return 0
    fi
  done
  return 1
}

list_disks() {
  local source_disk="$1" disk size_gb model
  log "Available disks:"
  for disk_path in /sys/block/sd*; do
    disk=$(basename "$disk_path")
    [ "/dev/$disk" = "$source_disk" ] && continue
    size_gb=$(($(cat "$disk_path/size") / 2097152))
    model=$(cat "$disk_path/device/model" 2>/dev/null) || model="unknown"
    printf '  /dev/%s (%d GB) [%s]\n' "$disk" "$size_gb" "$model" >&2
  done
}

confirm_erase() {
  local target_disk="$1"
  log "WARNING: This will erase all data on $target_disk"
  log "Type ERASE to confirm, or Ctrl+C to abort:"
  read -r response
  [ "$response" = "ERASE" ] || die "Cancelled by user"
}

create_gpt_layout() {
  local target="$1" total_sectors off idx

  log "Creating GPT layout on $target"

  if [ -f "$target" ]; then
    total_sectors=$(($(stat -c %s "$target") / 512))
  else
    total_sectors=$(cat "/sys/block/$(basename "$target")/size")
  fi

  run "sgdisk -o '$target' >/dev/null 2>&1"
  run "sgdisk -n 1:$ESP_START:$((ESP_START + ESP_SIZE - 1)) -c 1:EFI -t 1:EF00 '$target' >/dev/null 2>&1"

  off=$((ESP_START + ESP_SIZE))
  idx=2

  # Partition specs: name:size_sectors
  for spec in "misc:2048" "vbmeta_a:2048" "vbmeta_system_a:2048" "vbmeta_vendor_dlkm_a:2048" "vbmeta_system_dlkm_a:2048" "metadata:131072" "super:16777216" "boot_a:131072" "init_boot_a:16384" "vendor_boot_a:131072" "frp:2048"; do
    name=${spec%:*}
    size=${spec#*:}
    end=$((off + size - 1))
    run "sgdisk -n $idx:$off:$end -c $idx:$name -t $idx:8300 '$target' >/dev/null 2>&1"
    off=$((end + 1))
    idx=$((idx + 1))
  done

  # userdata takes the rest (sgdisk uses 0 to auto-extend to end)
  run "sgdisk -n $idx:$off:0 -c $idx:userdata -t $idx:8300 '$target' >/dev/null 2>&1"
}

copy_partition() {
  local source="$1" label="$2" target="$3"
  local src_start src_end tgt_start count

  log "Copying $label"

  # Match exact partition label (word boundary at end)
  src_start=$(sgdisk -p "$source" 2>/dev/null | awk "\$NF == \"$label\" {print \$2}")
  src_end=$(sgdisk -p "$source" 2>/dev/null | awk "\$NF == \"$label\" {print \$3}")
  tgt_start=$(sgdisk -p "$target" 2>/dev/null | awk "\$NF == \"$label\" {print \$2}")

  [ -z "$src_start" ] && die "Source partition $label not found"
  [ -z "$tgt_start" ] && die "Target partition $label not found"

  count=$((src_end - src_start + 1))
  run "dd if='$source' of='$target' bs=512 skip=$src_start seek=$tgt_start count=$count conv=notrunc,sparse status=none"
}

randomize_boot_guid() {
  local target="$1" new_uuid

  log "Generating new boot_a GUID"
  new_uuid=$(sgdisk -L "$target" 2>/dev/null | grep -A1 "^9" | grep "unique GUID" | awk '{print tolower($NF)}') || true
  if [ -z "$new_uuid" ]; then
    new_uuid=$(sgdisk -i 9 "$SOURCE_IMG" 2>/dev/null | grep "Partition unique GUID" | awk '{print tolower($4)}') || new_uuid="unknown"
  fi

  log "Setting boot_a GUID to $new_uuid"
  run "sgdisk -u '9:$new_uuid' '$target' >/dev/null 2>&1" || true

  log "Note: grub.cfg GUID update deferred to v2 (embedded in BOOTX64.EFI)"
}

main() {
  local source target source_disk

  log "Android-x86_64 Installer v1"

  if [ -n "$SOURCE_IMG" ] && [ -n "$TARGET_IMG" ]; then
    log "Test mode: source=$SOURCE_IMG target=$TARGET_IMG"
    [ -f "$SOURCE_IMG" ] || die "Source image not found: $SOURCE_IMG"
    source="$SOURCE_IMG"
    target="$TARGET_IMG"
  else
    log "Live mode"
    source_disk=$(detect_source_disk) || source_disk="/dev/sda"
    list_disks "$source_disk"
    log ""
    log "Enter target disk (e.g., /dev/sdb):"
    read -r target
    [ -b "$target" ] || die "Not a block device: $target"
    confirm_erase "$target"
    source="$source_disk"
  fi

  [ -e "$source" ] || die "Source not found: $source"
  [ "$DRY_RUN" = 1 ] || [ -e "$target" ] || die "Target not found: $target"

  log "Installing from $source to $target"
  create_gpt_layout "$target"

  for label in "misc" "vbmeta_a" "vbmeta_system_a" "vbmeta_vendor_dlkm_a" "vbmeta_system_dlkm_a" "metadata" "super" "boot_a" "init_boot_a" "vendor_boot_a" "frp" "EFI"; do
    copy_partition "$source" "$label" "$target"
  done

  randomize_boot_guid "$target"

  log "Installation complete"
  if [ -z "$TARGET_IMG" ]; then
    log "Boot from $target (EFI firmware menu, or insert as USB drive)"
  fi
}

main

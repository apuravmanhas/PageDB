#include "pagedb/wal.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#include "pagedb/byte_codec.h"
#include "pagedb/crc32c.h"

namespace pagedb {
namespace {

std::uint32_t compute_wal_header_crc32c(
    std::span<const std::byte, kWalFileHeaderSize> buf) noexcept {
  std::array<std::byte, kWalFileHeaderSize> copy{};
  std::memcpy(copy.data(), buf.data(), kWalFileHeaderSize);
  std::memset(copy.data() + kWalHeaderCrcOffset, 0, sizeof(std::uint32_t));
  return crc32c(copy);
}

std::uint32_t compute_wal_record_crc32c(
    std::span<const std::byte, kWalRecordCrcOffset> header_prefix,
    std::span<const std::byte> payload) noexcept {
  const std::uint32_t prefix_crc = crc32c(header_prefix);
  return crc32c_extend(prefix_crc, payload);
}

bool has_valid_record_after(std::span<const std::byte> wal_body,
                            std::size_t start_pos) noexcept {
  if (wal_body.size() < kWalRecordHeaderSize) {
    return false;
  }
  const std::size_t max_pos = wal_body.size() - kWalRecordHeaderSize;
  for (std::size_t pos = start_pos + 1; pos <= max_pos; ++pos) {
    const auto magic_res = read_u32_le_at(wal_body, pos);
    if (!magic_res.ok() || magic_res.value() != kWalRecordMagic) {
      continue;
    }
    std::span<const std::byte, kWalRecordHeaderSize> hdr_span(
        wal_body.data() + pos, kWalRecordHeaderSize);
    auto hdr_res = WalCodec::decode_record_header(hdr_span);
    if (!hdr_res.ok()) {
      continue;
    }
    const WalRecordHeader& hdr = hdr_res.value();
    const std::size_t total_len =
        kWalRecordHeaderSize + static_cast<std::size_t>(hdr.payload_len);
    if (pos + total_len > wal_body.size()) {
      continue;
    }
    const auto payload_span =
        wal_body.subspan(pos + kWalRecordHeaderSize, hdr.payload_len);
    if (WalCodec::verify_record(hdr, payload_span).ok()) {
      return true;
    }
  }
  return false;
}

}  // namespace

Status WalCodec::encode_file_header(
    const WalFileHeader& hdr,
    std::span<std::byte, kWalFileHeaderSize> out) noexcept {
  if (hdr.wal_magic != kWalFileMagic) {
    return Status::InvalidArgument("invalid WAL file magic");
  }
  if (hdr.format_major != kFormatMajorVersion ||
      hdr.format_minor != kFormatMinorVersion) {
    return Status::InvalidArgument("invalid WAL format version");
  }
  if (hdr.page_size != kPageSize || !hdr.base_lsn.is_valid() ||
      hdr.reserved != 0) {
    return Status::InvalidArgument("invalid WAL file header fields");
  }

  std::memset(out.data(), 0, kWalFileHeaderSize);
  ByteWriter w(out);
  (void)w.write_u64_le(hdr.wal_magic);
  (void)w.write_u16_le(hdr.format_major);
  (void)w.write_u16_le(hdr.format_minor);
  (void)w.write_u32_le(hdr.page_size);
  (void)w.write_u64_le(hdr.base_lsn.value);
  (void)w.write_u32_le(0U);
  (void)w.write_u32_le(hdr.reserved);

  const std::uint32_t crc = compute_wal_header_crc32c(out);
  return write_u32_le_at(out, kWalHeaderCrcOffset, crc);
}

Result<WalFileHeader> WalCodec::decode_file_header(
    std::span<const std::byte, kWalFileHeaderSize> buf) noexcept {
  ByteReader r(buf);
  const auto magic = r.read_u64_le().value();
  if (magic != kWalFileMagic) {
    return Status::Corruption("WAL file magic mismatch");
  }

  const auto major = r.read_u16_le().value();
  const auto minor = r.read_u16_le().value();
  if (major != kFormatMajorVersion || minor != kFormatMinorVersion) {
    return Status::UnsupportedFormatVersion("unsupported WAL format version");
  }

  const auto page_size = r.read_u32_le().value();
  const Lsn base_lsn(r.read_u64_le().value());
  const auto stored_crc = r.read_u32_le().value();
  const auto reserved = r.read_u32_le().value();

  const std::uint32_t computed_crc = compute_wal_header_crc32c(buf);
  if (stored_crc != computed_crc) {
    return Status::Corruption("WAL file header CRC32C mismatch");
  }
  if (page_size != kPageSize || !base_lsn.is_valid() || reserved != 0) {
    return Status::Corruption("invalid WAL file header fields");
  }

  WalFileHeader hdr{};
  hdr.wal_magic = magic;
  hdr.format_major = major;
  hdr.format_minor = minor;
  hdr.page_size = page_size;
  hdr.base_lsn = base_lsn;
  hdr.header_crc32c = stored_crc;
  hdr.reserved = reserved;
  return Result<WalFileHeader>::Ok(hdr);
}

Status WalCodec::encode_record(const WalRecordHeader& hdr,
                               std::span<const std::byte> payload,
                               std::span<std::byte> out) noexcept {
  if (hdr.record_magic != kWalRecordMagic) {
    return Status::InvalidArgument("invalid WAL record magic");
  }
  if (hdr.flags_reserved[0] != 0 || hdr.flags_reserved[1] != 0 ||
      hdr.flags_reserved[2] != 0) {
    return Status::InvalidArgument("WAL record flags_reserved must be zero");
  }
  if (!hdr.lsn.is_valid() || !hdr.txn_id.is_valid()) {
    return Status::InvalidArgument("WAL record requires valid Lsn and TxnId");
  }

  switch (hdr.record_type) {
    case WalRecordType::kBegin:
    case WalRecordType::kCommit:
    case WalRecordType::kAbort:
    case WalRecordType::kCheckpoint:
      if (hdr.page_id != kInvalidPageId || hdr.payload_len != 0 ||
          !payload.empty()) {
        return Status::InvalidArgument(
            "non-page WAL record must have kInvalidPageId and empty payload");
      }
      break;
    case WalRecordType::kPageImage:
      if (!hdr.page_id.is_valid() || hdr.payload_len != kPageSize ||
          payload.size() != kPageSize) {
        return Status::InvalidArgument(
            "PAGE_IMAGE WAL record requires valid PageId and 4096-byte payload");
      }
      break;
    default:
      return Status::InvalidArgument("unsupported WalRecordType");
  }

  if (out.size() != kWalRecordHeaderSize + payload.size()) {
    return Status::InvalidArgument("WAL output buffer size mismatch");
  }

  ByteWriter w(out.subspan(0, kWalRecordCrcOffset));
  (void)w.write_u32_le(hdr.record_magic);
  (void)w.write_u8(static_cast<std::uint8_t>(hdr.record_type));
  (void)w.write_u8(hdr.flags_reserved[0]);
  (void)w.write_u8(hdr.flags_reserved[1]);
  (void)w.write_u8(hdr.flags_reserved[2]);
  (void)w.write_u64_le(hdr.lsn.value);
  (void)w.write_u64_le(hdr.txn_id.value);
  (void)w.write_u32_le(hdr.page_id.value);
  (void)w.write_u32_le(hdr.payload_len);

  std::span<const std::byte, kWalRecordCrcOffset> prefix(
      out.data(), kWalRecordCrcOffset);
  const std::uint32_t crc = compute_wal_record_crc32c(prefix, payload);
  Status s = write_u32_le_at(out, kWalRecordCrcOffset, crc);
  if (!s.ok()) {
    return s;
  }

  if (!payload.empty()) {
    std::memcpy(out.data() + kWalRecordHeaderSize, payload.data(),
                payload.size());
  }
  return Status::Ok();
}

Result<WalRecordHeader> WalCodec::decode_record_header(
    std::span<const std::byte, kWalRecordHeaderSize> buf) noexcept {
  ByteReader r(buf);
  const auto magic = r.read_u32_le().value();
  if (magic != kWalRecordMagic) {
    return Status::Corruption("WAL record magic mismatch");
  }

  const auto raw_type = r.read_u8().value();
  std::array<std::uint8_t, 3> flags{};
  flags[0] = r.read_u8().value();
  flags[1] = r.read_u8().value();
  flags[2] = r.read_u8().value();
  const Lsn lsn(r.read_u64_le().value());
  const TxnId txn_id(r.read_u64_le().value());
  const PageId page_id(r.read_u32_le().value());
  const auto payload_len = r.read_u32_le().value();
  const auto crc = r.read_u32_le().value();

  if (flags[0] != 0 || flags[1] != 0 || flags[2] != 0) {
    return Status::Corruption("non-zero flags_reserved in WAL record header");
  }
  if (!lsn.is_valid() || !txn_id.is_valid()) {
    return Status::Corruption("invalid Lsn or TxnId in WAL record header");
  }

  WalRecordType type = WalRecordType::kInvalid;
  switch (raw_type) {
    case 1:
      type = WalRecordType::kBegin;
      break;
    case 2:
      type = WalRecordType::kPageImage;
      break;
    case 3:
      type = WalRecordType::kCommit;
      break;
    case 4:
      type = WalRecordType::kAbort;
      break;
    case 5:
      type = WalRecordType::kCheckpoint;
      break;
    default:
      return Status::Corruption("unknown WalRecordType in WAL record header");
  }

  if (type == WalRecordType::kPageImage) {
    if (!page_id.is_valid() || payload_len != kPageSize) {
      return Status::Corruption(
          "invalid PageId or payload_len for PAGE_IMAGE record");
    }
  } else {
    if (page_id != kInvalidPageId || payload_len != 0) {
      return Status::Corruption(
          "non-page WAL record has valid PageId or non-zero payload_len");
    }
  }

  WalRecordHeader hdr{};
  hdr.record_magic = magic;
  hdr.record_type = type;
  hdr.flags_reserved = flags;
  hdr.lsn = lsn;
  hdr.txn_id = txn_id;
  hdr.page_id = page_id;
  hdr.payload_len = payload_len;
  hdr.record_crc32c = crc;
  return Result<WalRecordHeader>::Ok(hdr);
}

Status WalCodec::verify_record(const WalRecordHeader& hdr,
                               std::span<const std::byte> payload) noexcept {
  if (payload.size() != hdr.payload_len) {
    return Status::Corruption("WAL record payload size mismatch");
  }

  std::array<std::byte, kWalRecordCrcOffset> prefix{};
  ByteWriter w(prefix);
  (void)w.write_u32_le(hdr.record_magic);
  (void)w.write_u8(static_cast<std::uint8_t>(hdr.record_type));
  (void)w.write_u8(hdr.flags_reserved[0]);
  (void)w.write_u8(hdr.flags_reserved[1]);
  (void)w.write_u8(hdr.flags_reserved[2]);
  (void)w.write_u64_le(hdr.lsn.value);
  (void)w.write_u64_le(hdr.txn_id.value);
  (void)w.write_u32_le(hdr.page_id.value);
  (void)w.write_u32_le(hdr.payload_len);

  const std::uint32_t computed_crc = compute_wal_record_crc32c(prefix, payload);
  if (computed_crc != hdr.record_crc32c) {
    return Status::Corruption("WAL record CRC32C mismatch");
  }

  if (hdr.record_type == WalRecordType::kPageImage) {
    std::span<const std::byte, kPageSize> page_span(payload.data(), kPageSize);
    auto page_hdr_res =
        PageCodec::decode_and_verify_header(page_span, hdr.page_id);
    if (!page_hdr_res.ok()) {
      return page_hdr_res.status();
    }
    const PageHeader& page_hdr = page_hdr_res.value();
    if (page_hdr.page_lsn != hdr.lsn) {
      return Status::Corruption(
          "PAGE_IMAGE embedded page_lsn does not match WAL record Lsn");
    }
    if (hdr.page_id == kSuperblockPageId) {
      auto sb_res = PageCodec::decode_superblock(page_span);
      if (!sb_res.ok()) {
        return sb_res.status();
      }
    } else if (page_hdr.page_type == PageType::kLeaf ||
               page_hdr.page_type == PageType::kInternal) {
      Status layout_s =
          PageCodec::validate_btree_page_layout(page_span, hdr.page_id);
      if (!layout_s.ok()) {
        return layout_s;
      }
    } else if (page_hdr.page_type == PageType::kFree) {
      auto free_res = PageCodec::decode_free_page(page_span, hdr.page_id);
      if (!free_res.ok()) {
        return free_res.status();
      }
    } else {
      return Status::Corruption("non-zero PageId carries Superblock PageType");
    }
  }
  return Status::Ok();
}

Result<std::unique_ptr<Wal>> Wal::open(const std::filesystem::path& wal_path,
                                       const PosixIoOps* io_ops) {
  if (io_ops == nullptr) {
    return Status::InvalidArgument("Wal::open io_ops must not be null");
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(wal_path, ec);
  if (ec) {
    return Status::IoError("failed to stat WAL file: " + ec.message());
  }

  auto wal = std::make_unique<Wal>();
  wal->wal_path_ = wal_path;

  if (!exists) {
    auto fh_res = FileHandle::open(wal_path, OpenMode::kCreateNew, io_ops);
    if (!fh_res.ok()) {
      return fh_res.status();
    }
    wal->file_ = std::move(fh_res.value());

    WalFileHeader hdr{};
    hdr.base_lsn = Lsn(1);
    std::array<std::byte, kWalFileHeaderSize> hdr_buf{};
    Status es = WalCodec::encode_file_header(hdr, hdr_buf);
    if (!es.ok()) {
      return es;
    }
    Status ws = wal->file_.write_exact_at(0, hdr_buf);
    if (!ws.ok()) {
      return ws;
    }
    Status ss = wal->file_.sync_data();
    if (!ss.ok()) {
      return ss;
    }
    Status ds = FileHandle::sync_directory(wal_path.parent_path());
    if (!ds.ok()) {
      return ds;
    }

    auto dec_res = WalCodec::decode_file_header(hdr_buf);
    if (!dec_res.ok()) {
      return dec_res.status();
    }
    wal->header_ = dec_res.value();
    wal->write_offset_ = kWalFileHeaderSize;
    wal->next_lsn_ = wal->header_.base_lsn;
    wal->next_txn_id_ = TxnId(1);
    return Result<std::unique_ptr<Wal>>::Ok(std::move(wal));
  }

  auto fh_res = FileHandle::open(wal_path, OpenMode::kReadWrite, io_ops);
  if (!fh_res.ok()) {
    return fh_res.status();
  }
  wal->file_ = std::move(fh_res.value());

  auto sz_res = wal->file_.size_bytes();
  if (!sz_res.ok()) {
    return sz_res.status();
  }
  const std::uint64_t file_size = sz_res.value();

  // If the file is smaller than the 32-byte header, creation was interrupted
  // before any transaction record could be written. Reinitialize the header.
  if (file_size < kWalFileHeaderSize) {
    Status ts = wal->file_.truncate(0);
    if (!ts.ok()) {
      return ts;
    }
    WalFileHeader hdr{};
    hdr.base_lsn = Lsn(1);
    std::array<std::byte, kWalFileHeaderSize> hdr_buf{};
    Status es = WalCodec::encode_file_header(hdr, hdr_buf);
    if (!es.ok()) {
      return es;
    }
    Status ws = wal->file_.write_exact_at(0, hdr_buf);
    if (!ws.ok()) {
      return ws;
    }
    Status ss = wal->file_.sync_data();
    if (!ss.ok()) {
      return ss;
    }
    auto dec_res = WalCodec::decode_file_header(hdr_buf);
    if (!dec_res.ok()) {
      return dec_res.status();
    }
    wal->header_ = dec_res.value();
    wal->write_offset_ = kWalFileHeaderSize;
    wal->next_lsn_ = wal->header_.base_lsn;
    wal->next_txn_id_ = TxnId(1);
    return Result<std::unique_ptr<Wal>>::Ok(std::move(wal));
  }

  std::array<std::byte, kWalFileHeaderSize> hdr_buf{};
  Status rs = wal->file_.read_exact_at(0, hdr_buf);
  if (!rs.ok()) {
    return rs;
  }
  auto dec_res = WalCodec::decode_file_header(hdr_buf);
  if (!dec_res.ok()) {
    return dec_res.status();
  }
  wal->header_ = dec_res.value();
  wal->write_offset_ = file_size;
  wal->next_lsn_ = wal->header_.base_lsn;
  wal->next_txn_id_ = TxnId(1);
  return Result<std::unique_ptr<Wal>>::Ok(std::move(wal));
}

Result<WalRecoveryReport> Wal::recover() {
  if (!file_.is_open()) {
    return Status::InvalidArgument("Wal::recover on closed WAL");
  }

  auto sz_res = file_.size_bytes();
  if (!sz_res.ok()) {
    return sz_res.status();
  }
  const std::uint64_t file_size = sz_res.value();
  if (file_size < kWalFileHeaderSize) {
    return Status::Corruption("WAL file shorter than header during recover");
  }

  const std::size_t body_size =
      static_cast<std::size_t>(file_size - kWalFileHeaderSize);
  std::vector<std::byte> body(body_size);
  if (body_size > 0) {
    Status rs = file_.read_exact_at(kWalFileHeaderSize, body);
    if (!rs.ok()) {
      return rs;
    }
  }

  WalRecoveryReport report{};
  report.base_lsn = header_.base_lsn;
  report.max_committed_lsn = kInvalidLsn;

  Lsn expected_lsn = header_.base_lsn;
  Lsn last_finished_next_lsn = header_.base_lsn;
  TxnId max_committed_txn_id = kInvalidTxnId;
  std::uint64_t last_finished_end_offset = kWalFileHeaderSize;

  TxnId active_txn_id = kInvalidTxnId;
  std::vector<WalPageUpdate> active_txn_updates;
  std::set<TxnId> finished_txns;

  std::size_t pos = 0;
  bool hit_torn_record = false;

  while (pos < body.size()) {
    const std::size_t remaining = body.size() - pos;
    if (remaining < kWalRecordHeaderSize) {
      hit_torn_record = true;
      break;
    }

    std::span<const std::byte, kWalRecordHeaderSize> hdr_span(
        body.data() + pos, kWalRecordHeaderSize);
    auto hdr_res = WalCodec::decode_record_header(hdr_span);
    if (!hdr_res.ok()) {
      if (has_valid_record_after(body, pos)) {
        return Status::Corruption(
            "corrupt WAL record header precedes later valid WAL record");
      }
      hit_torn_record = true;
      break;
    }

    const WalRecordHeader& hdr = hdr_res.value();
    const std::size_t rec_total_len =
        kWalRecordHeaderSize + static_cast<std::size_t>(hdr.payload_len);
    if (remaining < rec_total_len) {
      if (has_valid_record_after(body, pos)) {
        return Status::Corruption(
            "truncated WAL record precedes later valid WAL record");
      }
      hit_torn_record = true;
      break;
    }

    const auto payload_span =
        std::span<const std::byte>(body).subspan(pos + kWalRecordHeaderSize,
                                                 hdr.payload_len);
    Status vs = WalCodec::verify_record(hdr, payload_span);
    if (!vs.ok()) {
      if (has_valid_record_after(body, pos)) {
        return Status::Corruption(
            "corrupt WAL record CRC precedes later valid WAL record");
      }
      hit_torn_record = true;
      break;
    }

    // Checksum is valid; enforce strict LSN monotonicity and transaction state.
    if (hdr.lsn != expected_lsn) {
      return Status::Corruption("non-monotonic or out-of-sequence Lsn in WAL");
    }
    expected_lsn = Lsn(expected_lsn.value + 1ULL);

    switch (hdr.record_type) {
      case WalRecordType::kBegin:
        if (active_txn_id.is_valid() || finished_txns.contains(hdr.txn_id)) {
          return Status::Corruption(
              "invalid BEGIN record in WAL (overlapping or duplicate TxnId)");
        }
        if (max_committed_txn_id.is_valid() &&
            hdr.txn_id.value <= max_committed_txn_id.value) {
          return Status::Corruption("non-monotonic TxnId in WAL BEGIN record");
        }
        active_txn_id = hdr.txn_id;
        active_txn_updates.clear();
        break;

      case WalRecordType::kPageImage: {
        if (!active_txn_id.is_valid() || hdr.txn_id != active_txn_id) {
          return Status::Corruption(
              "PAGE_IMAGE record in WAL does not match active transaction");
        }
        WalPageUpdate upd{};
        upd.lsn = hdr.lsn;
        upd.txn_id = hdr.txn_id;
        upd.page_id = hdr.page_id;
        std::memcpy(upd.page_image.data(), payload_span.data(), kPageSize);
        active_txn_updates.push_back(upd);
        break;
      }

      case WalRecordType::kCommit:
        if (!active_txn_id.is_valid() || hdr.txn_id != active_txn_id) {
          return Status::Corruption(
              "COMMIT record in WAL does not match active transaction");
        }
        for (auto& upd : active_txn_updates) {
          report.committed_updates.push_back(std::move(upd));
        }
        active_txn_updates.clear();
        finished_txns.insert(active_txn_id);
        max_committed_txn_id = active_txn_id;
        active_txn_id = kInvalidTxnId;
        report.max_committed_lsn = hdr.lsn;
        last_finished_next_lsn = expected_lsn;
        last_finished_end_offset = kWalFileHeaderSize + pos + rec_total_len;
        break;

      case WalRecordType::kAbort:
        if (!active_txn_id.is_valid() || hdr.txn_id != active_txn_id) {
          return Status::Corruption(
              "ABORT record in WAL does not match active transaction");
        }
        active_txn_updates.clear();
        finished_txns.insert(active_txn_id);
        if (!max_committed_txn_id.is_valid() ||
            active_txn_id.value > max_committed_txn_id.value) {
          max_committed_txn_id = active_txn_id;
        }
        active_txn_id = kInvalidTxnId;
        last_finished_next_lsn = expected_lsn;
        last_finished_end_offset = kWalFileHeaderSize + pos + rec_total_len;
        break;

      case WalRecordType::kCheckpoint:
        if (active_txn_id.is_valid()) {
          return Status::Corruption(
              "CHECKPOINT record during active transaction in WAL");
        }
        last_finished_next_lsn = expected_lsn;
        last_finished_end_offset = kWalFileHeaderSize + pos + rec_total_len;
        break;

      default:
        return Status::Corruption("unexpected WalRecordType in WAL scan");
    }

    pos += rec_total_len;
  }

  // If the tail had an incomplete record or an open uncommitted transaction,
  // truncate the WAL back to the end of the last completed transaction.
  if (hit_torn_record || active_txn_id.is_valid() ||
      last_finished_end_offset < file_size) {
    Status ts = file_.truncate(last_finished_end_offset);
    if (!ts.ok()) {
      return ts;
    }
    Status ss = file_.sync_data();
    if (!ss.ok()) {
      return ss;
    }
    report.truncated_uncommitted_tail = true;
  }

  write_offset_ = last_finished_end_offset;
  next_lsn_ = last_finished_next_lsn;
  next_txn_id_ = max_committed_txn_id.is_valid()
                     ? TxnId(max_committed_txn_id.value + 1ULL)
                     : TxnId(1);
  max_committed_lsn_ = report.max_committed_lsn;

  report.next_lsn = next_lsn_;
  report.next_txn_id = next_txn_id_;
  report.valid_end_offset = write_offset_;
  return Result<WalRecoveryReport>::Ok(std::move(report));
}

Result<Lsn> Wal::append_empty_record(WalRecordType type, TxnId txn_id) {
  if (!file_.is_open()) {
    return Status::InvalidArgument("append on closed WAL");
  }
  const Lsn lsn = next_lsn_;
  WalRecordHeader hdr{};
  hdr.record_type = type;
  hdr.lsn = lsn;
  hdr.txn_id = txn_id;
  hdr.page_id = kInvalidPageId;
  hdr.payload_len = 0;

  std::array<std::byte, kWalRecordHeaderSize> rec_buf{};
  Status es = WalCodec::encode_record(hdr, {}, rec_buf);
  if (!es.ok()) {
    return es;
  }

  Status ws = file_.write_exact_at(write_offset_, rec_buf);
  if (!ws.ok()) {
    return ws;
  }
  write_offset_ += kWalRecordHeaderSize;
  next_lsn_ = Lsn(next_lsn_.value + 1ULL);
  if (type == WalRecordType::kCommit) {
    max_committed_lsn_ = lsn;
  }
  return Result<Lsn>::Ok(lsn);
}

Result<Lsn> Wal::append_begin(TxnId txn_id) {
  return append_empty_record(WalRecordType::kBegin, txn_id);
}

Result<Lsn> Wal::append_commit(TxnId txn_id) {
  return append_empty_record(WalRecordType::kCommit, txn_id);
}

Result<Lsn> Wal::append_abort(TxnId txn_id) {
  return append_empty_record(WalRecordType::kAbort, txn_id);
}

Result<Lsn> Wal::append_checkpoint() {
  const TxnId sys_txn(next_txn_id_.value);
  return append_empty_record(WalRecordType::kCheckpoint, sys_txn);
}

Result<Lsn> Wal::append_page_image(
    TxnId txn_id, PageId page_id,
    std::span<std::byte, kPageSize> in_out_page_image) {
  if (!file_.is_open()) {
    return Status::InvalidArgument("append_page_image on closed WAL");
  }
  const Lsn lsn = next_lsn_;
  Status stamp_s = PageCodec::stamp_lsn(in_out_page_image, lsn);
  if (!stamp_s.ok()) {
    return stamp_s;
  }

  WalRecordHeader hdr{};
  hdr.record_type = WalRecordType::kPageImage;
  hdr.lsn = lsn;
  hdr.txn_id = txn_id;
  hdr.page_id = page_id;
  hdr.payload_len = static_cast<std::uint32_t>(kPageSize);

  std::vector<std::byte> rec_buf(kWalRecordHeaderSize + kPageSize);
  Status es = WalCodec::encode_record(hdr, in_out_page_image, rec_buf);
  if (!es.ok()) {
    return es;
  }

  Status ws = file_.write_exact_at(write_offset_, rec_buf);
  if (!ws.ok()) {
    return ws;
  }
  write_offset_ += rec_buf.size();
  next_lsn_ = Lsn(next_lsn_.value + 1ULL);
  return Result<Lsn>::Ok(lsn);
}

Status Wal::sync() {
  if (!file_.is_open()) {
    return Status::InvalidArgument("sync on closed WAL");
  }
  return file_.sync_data();
}

Status Wal::reset(Lsn new_base_lsn) {
  if (!file_.is_open()) {
    return Status::InvalidArgument("reset on closed WAL");
  }
  if (!new_base_lsn.is_valid()) {
    return Status::InvalidArgument("reset with invalid Lsn");
  }

  WalFileHeader hdr{};
  hdr.base_lsn = new_base_lsn;
  std::array<std::byte, kWalFileHeaderSize> hdr_buf{};
  Status es = WalCodec::encode_file_header(hdr, hdr_buf);
  if (!es.ok()) {
    return es;
  }

  Status ws = file_.write_exact_at(0, hdr_buf);
  if (!ws.ok()) {
    return ws;
  }
  Status ts = file_.truncate(kWalFileHeaderSize);
  if (!ts.ok()) {
    return ts;
  }
  Status ss = file_.sync_data();
  if (!ss.ok()) {
    return ss;
  }

  header_ = hdr;
  write_offset_ = kWalFileHeaderSize;
  next_lsn_ = new_base_lsn;
  max_committed_lsn_ = kInvalidLsn;
  return Status::Ok();
}

void Wal::discard_unsynced_from(std::uint64_t offset, Lsn reset_next_lsn,
                                TxnId reset_next_txn_id,
                                Lsn prev_max_committed_lsn) noexcept {
  if (file_.is_open()) {
    (void)file_.truncate(offset);
  }
  write_offset_ = offset;
  next_lsn_ = reset_next_lsn;
  next_txn_id_ = reset_next_txn_id;
  max_committed_lsn_ = prev_max_committed_lsn;
}

Status Wal::close() noexcept { return file_.close(); }

}  // namespace pagedb

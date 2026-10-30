#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

#include "common/alignment.h"
#include "common/assert.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <mutex>
#include <compare>
#include <limits>
#include <atomic>
#include <optional>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
class TileManager;
class CommandRecorder;
struct ImageTestAccess;

using ImageId = Common::SlotId;

struct CachedImageView {
	ImageViewInfo info;
	vk::ImageView view = nullptr;
};

struct ImageUsage {
	bool texture       = false;
	bool storage       = false;
	bool render_target = false;
	bool depth_target  = false;
	bool video_out     = false;
};

struct ImageBinding {
	vk::ImageLayout  attachment_layout = vk::ImageLayout::eUndefined;
	vk::AccessFlags2 attachment_access;
	bool             is_bound      = false;
	bool             is_target     = false;
	bool             needs_rebind  = false;
	bool             force_general = false;
	bool             shader_write  = false;
};

// Round 167 (images:lean): while one lives on the GPU thread, the image uses made are a draw's or
// a dispatch's (its render targets and textures, RenderExecutor), which the guest's own sync
// points order. Not an emulator-made copy, blit, clear or conversion.
class GuestImageUses final {
public:
	GuestImageUses() noexcept: m_previous(t_active) { t_active = true; }
	~GuestImageUses() { t_active = m_previous; }
	GuestImageUses(const GuestImageUses&)                      = delete;
	GuestImageUses&           operator=(const GuestImageUses&) = delete;
	[[nodiscard]] static bool Active() noexcept { return t_active; }

private:
	friend class ImageStateOverlay;
	static inline thread_local bool t_active = false;
	bool                            m_previous;
};

class Image;
class CommandScheduler;

// Parallel execution slice 2, part 2 (KYTY_CHUNK_LISTS=1; docs/GT7-PARALLEL-AUDIT.md): image
// states per chunk. While one is active on a thread, Image::GetBarriers works on its copy of each
// image's state instead of the image's: an image's first use in the chunk (per subresource) is
// assumed to find the image as it needs it (no barrier) and is noted; later uses make their
// barriers against the copy. Stitch, before the chunk's commands join the stream, records the
// barriers that bring each image from its real state to those first needs and makes the copy's
// final state the real one. A chunk recorded this way needs no image state from the chunks
// before it. The barriers go into the chunk's own commands: its first first use records a
// placeholder there (outside rendering), which Stitch fills before the commands are recorded, so
// they are always inside a command buffer (a chunk's commands may begin with one).
class ImageStateOverlay {
public:
	[[nodiscard]] static ImageStateOverlay* Active() noexcept { return t_active; }
	void                                    Activate() noexcept { t_active = this; }
	static void                             Deactivate() noexcept { t_active = nullptr; }
	[[nodiscard]] bool                      Empty() const noexcept { return m_entries.empty(); }
	// With `verify`, also replays each image's uses on its real state in order and counts where
	// the result differs from the stitched one.
	void Stitch(bool verify);
	// Keep each use for Stitch's verify.
	bool record_calls = false;
	// The image is going away: its entry is dropped.
	void Forget(const Image* image);

	struct Counts {
		uint64_t stitches = 0, images = 0, firsts = 0, barriers = 0, chunk_barriers = 0;
		uint64_t verify_images = 0, verify_layouts = 0, verify_accesses = 0;
	};
	[[nodiscard]] static Counts& GetCounts() noexcept;

private:
	friend class Image;
	struct Call {
		vk::ImageLayout                      layout;
		vk::AccessFlags2                     access;
		vk::PipelineStageFlags2              stage;
		std::optional<ImageSubresourceRange> range;
		bool                                 guest = false;
	};
	struct First {
		int32_t          index = -1; // a subresource (level * layers + layer), or -1: the image
		VulkanImageState need;
		bool             guest = false;
	};
	struct Entry {
		Image*                        image            = nullptr;
		bool                          whole_known      = false;
		bool                          whole_barriered  = false;
		VulkanImageState              whole;
		bool                          has_sub          = false;
		std::vector<VulkanImageState> sub;
		std::vector<uint8_t>          known;
		std::vector<uint8_t>          barriered;
		std::vector<First>            firsts;
		std::vector<Call>             calls; // verify only
	};
	Entry& Get(Image& image);

	// Before a chunk's first first use (Image::OverlayBarriers): the placeholder's barriers.
	void RecordPlaceholder(CommandScheduler& scheduler);

	std::vector<Entry>                       m_entries;
	std::shared_ptr<std::vector<vk::ImageMemoryBarrier2>> m_pending;
	std::unordered_map<const Image*, size_t> m_index;
	static inline thread_local ImageStateOverlay* t_active = nullptr;
};

class Image final {
public:
	Image(GraphicContext& graphics, CommandScheduler& scheduler, const ImageInfo& info);
	~Image();
	KYTY_CLASS_NO_COPY(Image);

	[[nodiscard]] vk::ImageView FindView(const ImageViewInfo& view_info);
	using Barriers = std::vector<vk::ImageMemoryBarrier2>;
	// The layout its last use left it in (the active ImageStateOverlay's, in a chunk).
	[[nodiscard]] vk::ImageLayout CurrentLayout() const;
	[[nodiscard]] Barriers GetBarriers(vk::ImageLayout                      destination_layout,
	                                   vk::AccessFlags2                     destination_access,
	                                   vk::PipelineStageFlags2              destination_stage,
	                                   std::optional<ImageSubresourceRange> range);
	// Records through the recording thread when given a threaded CommandBuffer::Recorder(); a raw
	// command buffer converts.
	void Transit(vk::ImageLayout destination_layout, vk::AccessFlags2 destination_access,
	             std::optional<ImageSubresourceRange> range, const CommandRecorder& command_buffer);
	void Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	            uint64_t size);
	void Download(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, uint64_t offset,
	              uint64_t size);
	void CopyImage(Image& source);
	void Resolve(Image& source, const ImageSubresourceRange& source_range,
	             const ImageSubresourceRange& destination_range);
	void CopyImageWithBuffer(Image& source, Buffer& buffer, TileManager& tiler);
	void CopyMip(Image& source, uint32_t mip, uint32_t layer);

	void InvalidateCpuWrite(uint64_t vaddr, uint64_t size) {
		if (ImageRangeOverlaps(info.data.address, info.data.size, vaddr, size)) {
			m_cpu_dirty        = true;
			m_maybe_cpu_dirty  = false;
			m_maybe_hash_valid = false;
		} else if (ImagePageRangesOverlap(info.data.address, info.data.size, vaddr, size)) {
			m_maybe_cpu_dirty = true;
		}
	}

	[[nodiscard]] bool IsCpuDirty() const { return m_cpu_dirty || m_maybe_cpu_dirty; }
	[[nodiscard]] bool IsDefinitelyCpuDirty() const { return m_cpu_dirty; }
	[[nodiscard]] bool IsMaybeCpuDirty() const { return m_maybe_cpu_dirty; }
	void               MarkMaybeCpuDirty() {
		if (!m_cpu_dirty) {
			m_maybe_cpu_dirty = true;
		}
	}
	[[nodiscard]] bool NeedsMaybeCpuHash() const {
		return m_maybe_cpu_dirty && !m_maybe_hash_valid;
	}
	void SetMaybeCpuHash(uint64_t hash) {
		if (!NeedsMaybeCpuHash()) {
			EXIT("image cannot initialize maybe-dirty hash\n");
		}
		m_maybe_cpu_hash   = hash;
		m_maybe_hash_valid = true;
	}
	[[nodiscard]] bool ResolveMaybeCpuHash(uint64_t hash) {
		if (!m_maybe_cpu_dirty || !m_maybe_hash_valid || m_cpu_dirty) {
			EXIT("image cannot resolve maybe-dirty hash\n");
		}
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
		m_cpu_dirty |= hash != m_maybe_cpu_hash;
		return m_cpu_dirty;
	}

	// TextureCache::RefreshByContent: the guest bytes changed (definitely dirty), or they did not
	// (a maybe-dirty mark is dropped).
	void MarkCpuDirty() {
		m_cpu_dirty        = true;
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
	}
	void ClearMaybeCpuDirty() {
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
	}

	void RefreshComplete() {
		if (!IsCpuDirty()) {
			EXIT("clean image cannot complete a refresh\n");
		}
		m_cpu_dirty        = false;
		m_maybe_cpu_dirty  = false;
		m_maybe_hash_valid = false;
	}

	[[nodiscard]] bool IsGpuModified() const noexcept { return m_gpu_modified; }
	void               MarkGpuModified() noexcept { m_gpu_modified = true; }
	void               ClearGpuModified() noexcept { m_gpu_modified = false; }

	[[nodiscard]] bool IsBufferModified() const noexcept { return m_buffer_modified; }
	void               MarkBufferModified() noexcept { m_buffer_modified = true; }
	void               ClearBufferModified() noexcept { m_buffer_modified = false; }

	[[nodiscard]] bool Overlaps(uint64_t address, uint64_t size,
	                            bool pages = false) const noexcept {
		return pages ? ImagePageRangesOverlap(info.data.address, info.data.size, address, size)
		             : ImageRangeOverlaps(info.data.address, info.data.size, address, size);
	}
	[[nodiscard]] bool SafeToDownload() const noexcept {
		return IsGpuModified() && !IsBufferModified() && !IsCpuDirty();
	}
	[[nodiscard]] bool IsTracked() const noexcept { return track_addr != 0 && track_addr_end != 0; }
	[[nodiscard]] uint64_t AccountedSize() const noexcept {
		return backing.image == nullptr ? 0 : Common::AlignUp(info.data.size, 1024);
	}
	[[nodiscard]] uint64_t HashGuestEdges() const;

	ImageInfo        info;
	VulkanImage      backing;
	std::vector<CachedImageView> views;
	ImageUsage       usage;
	ImageBinding     binding;
	bool             registered     = false;
	// Referenced by a bindless table slot: the collector keeps it, and unregistering it tells the
	// table (TextureCache::on_bindless_unregister) to repoint the slot.
	bool             bindless_pinned = false;
	ImageId          self_id {}; // set by TextureCache::InsertImage
	mutable uint32_t query_epoch    = 0;
	uint64_t         track_addr     = 0;
	uint64_t         track_addr_end = 0;
	ImageId          depth_id {};
	// A depth image's stencil associations (the images whose depth_id is it): deleting it frees
	// them without a scan of the whole cache (upstream's "avoid full-cache scans when deleting
	// stencil associations").
	std::vector<ImageId> stencil_ids;
	// The current stencil plane's mapping into the depth image, also retained by its association.
	ImageSubresourceRange stencil_subresources;
	uint64_t         tick_accessed_last = 0;
	// Presented frame of the last access: overlapping aliases age by frames as well as by ticks
	// (TextureCache::ResolveOverlap, KYTY_IMAGE_ALIAS_AGE).
	uint64_t         frame_accessed_last = 0;
	uint64_t         frame_guest_dropped = 0; // when the guest last dropped it from its heaps
	size_t           lru_id             = 0;
	// The LRU tick the cache last gave it (round 80): a touch in the same tick skips the list's
	// node, a cache miss on most of the draws' textures.
	uint64_t lru_tick = 0;
	// Last GPU writer among live overlapping aliases; cleared when another alias takes the bytes.
	bool             alias_owner        = false;
	// Kept beside an image of another kind at its address, neither written by the GPU
	// (TextureCache::KeepReadOnlyAlias, round 79): a GPU write to one of them frees the others.
	bool read_only_alias = false;
	// CommandScheduler tick, and metadata slices, MaterializeDccClear last resolved this image's
	// DCC metadata for. UINT64_MAX means never checked. Lets repeated FindImage lookups within
	// the same still-unsubmitted recording (i.e. many draws to the same bound render target) skip
	// the synchronous GPU readback after the first one -- a real re-clear in a later recording
	// still gets caught because the tick will have advanced by then.
	uint64_t         dcc_clear_checked_tick  = UINT64_MAX;
	uint32_t         dcc_clear_checked_first = 0;
	uint32_t         dcc_clear_checked_count = 0;
	// A small linear image the CPU writes (TextureCache::ChecksGuestContent): the hash of the
	// guest bytes its last upload took, and the guest submission they were last compared in.
	uint64_t content_hash             = 0;
	uint64_t content_check_submission = 0;
	bool     content_hash_valid       = false;
	// TextureCache::InitializeImage has run: it holds contents (an upload, or the GPU's) since.
	bool initialized = false;
	// It holds pixels the GPU wrote (a draw, store, clear or copy) since its last upload.
	bool gpu_pixels = false;
	// Counts the uploads and GPU writes (TextureCache::ContentSerial): diagnostics scan an image
	// again only once it changed.
	uint64_t content_serial = 0;
	// TextureCache::ResetUnmappedDepth: the content it last checked (UINT64_MAX: none yet).
	uint64_t sparse_checked_serial = UINT64_MAX;
	// Round 167 (images:lean): an emulator-made use (a copy, blit, clear, conversion) since its
	// last barrier, which no guest sync point orders: the next use makes its barrier.
	bool internal_use = false;

private:
	friend class ImageStateOverlay;
	// GetBarriers on the active ImageStateOverlay's copy of the state.
	[[nodiscard]] Barriers OverlayBarriers(ImageStateOverlay& overlay,
	                                       vk::ImageLayout destination_layout,
	                                       vk::AccessFlags2 destination_access,
	                                       vk::PipelineStageFlags2 destination_stage,
	                                       std::optional<ImageSubresourceRange> range);
	friend struct ImageTestAccess;

	[[nodiscard]] static vk::ImageAspectFlags FullAspectMask(vk::Format format) noexcept;
	[[nodiscard]] static uint32_t             CopyRows(uint64_t row_size, uint32_t rows,
	                                                   uint64_t capacity) noexcept;
	[[nodiscard]] static std::pair<uint32_t, uint32_t>
	SanitizeCopyLayers(const Image& source, const Image& destination, uint32_t depth);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	uint64_t          m_maybe_cpu_hash   = 0;
	bool              m_cpu_dirty        = false;
	bool              m_maybe_cpu_dirty  = false;
	bool              m_maybe_hash_valid = false;
	bool              m_gpu_modified     = false;
	bool              m_buffer_modified  = false;
};

// Changes of a bindless-pinned image's state that the per-draw bindless pass checks
// (RenderExecutor::CommitBindings): its layout, render-target use and registration. A heap is
// rechecked only when this changed since it was last checked, or for the images resolved since.
inline std::atomic<uint64_t> g_bindless_state_generation {1};
// The pinned images whose state changed, for the engine method's per-draw pass (E5), which looks
// at these and at newly settled images only, instead of every image of a heap.
struct BindlessStateChanges {
	std::mutex           mutex;
	std::vector<ImageId> ids;
};
inline BindlessStateChanges g_bindless_state_changes;
inline void NoteBindlessStateChange(const Image& image) noexcept {
	if (image.bindless_pinned) {
		g_bindless_state_generation.fetch_add(1, std::memory_order_relaxed);
		std::scoped_lock lock {g_bindless_state_changes.mutex};
		g_bindless_state_changes.ids.push_back(image.self_id);
	}
}

namespace ImageOps {

void                                 Validate(const ImageInfo& info);
[[nodiscard]] Prospero::BufferFormat RenderTargetTransferFormat(uint32_t bytes_per_element);

} // namespace ImageOps

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_H_

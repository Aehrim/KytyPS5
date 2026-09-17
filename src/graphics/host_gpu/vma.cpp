#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <cstdlib>
#include <mutex>
#include <vector>
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <cinttypes>

namespace Libs::Graphics {

namespace {

// Titles alias transient render targets in one guest heap, so the texture cache drops and
// recreates the same few images every frame (Demon's Souls: eight per frame, one of them
// 64 MiB). vkAllocateMemory and vkFreeMemory for those cost a fifth of the GPU thread. Released
// images wait here and are handed out again for an identical create info; their contents are
// undefined, exactly like those of a new image.
class ImageRecycler {
public:
	struct Entry {
		vk::Format           format {};
		vk::ImageType        image_type {};
		vk::Extent3D         extent {};
		uint32_t             layers     = 0;
		uint32_t             mip_levels = 0;
		uint32_t             samples    = 0;
		vk::ImageUsageFlags  usage {};
		vk::ImageCreateFlags flags {};
		VkImage              image      = VK_NULL_HANDLE;
		VmaAllocation        allocation = nullptr;
		uint64_t             size       = 0;
		uint64_t             stamp      = 0;
	};

	[[nodiscard]] static bool Enabled() noexcept {
		static const bool enabled = std::getenv("KYTY_NO_IMAGE_POOL") == nullptr;
		return enabled;
	}

	bool Take(const vk::ImageCreateInfo& info, Entry& out) {
		std::scoped_lock lock(m_mutex);
		for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
			if (it->format == info.format && it->image_type == info.imageType &&
			    it->extent == info.extent && it->layers == info.arrayLayers &&
			    it->mip_levels == info.mipLevels &&
			    it->samples == static_cast<uint32_t>(info.samples) && it->usage == info.usage &&
			    it->flags == info.flags) {
				out = *it;
				m_bytes -= it->size;
				m_entries.erase(it);
				return true;
			}
		}
		return false;
	}

	// Entries that fall out of the pool are returned for destruction.
	void Put(Entry entry, std::vector<Entry>& evicted) {
		std::scoped_lock lock(m_mutex);
		entry.stamp = ++m_stamp;
		m_bytes += entry.size;
		m_entries.push_back(entry);
		while (!m_entries.empty() && (m_entries.size() > MaxEntries || m_bytes > MaxBytes ||
		                              m_stamp - m_entries.front().stamp > MaxAge)) {
			m_bytes -= m_entries.front().size;
			evicted.push_back(m_entries.front());
			m_entries.erase(m_entries.begin());
		}
	}

	void Drain(std::vector<Entry>& evicted) {
		std::scoped_lock lock(m_mutex);
		evicted.insert(evicted.end(), m_entries.begin(), m_entries.end());
		m_entries.clear();
		m_bytes = 0;
	}

private:
	static constexpr size_t   MaxEntries = 64;
	static constexpr uint64_t MaxBytes   = 768ull * 1024 * 1024;
	static constexpr uint64_t MaxAge     = 512; // releases

	std::mutex         m_mutex;
	std::vector<Entry> m_entries;
	uint64_t           m_bytes = 0;
	uint64_t           m_stamp = 0;
};

ImageRecycler g_image_recycler;

} // namespace

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	std::vector<ImageRecycler::Entry> pooled;
	g_image_recycler.Drain(pooled);
	for (const auto& old: pooled) {
		vmaDestroyImage(allocator, old.image, old.allocation);
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	if (ImageRecycler::Entry recycled; ImageRecycler::Enabled() && image_info.pNext == nullptr &&
	                                   image_info.initialLayout == vk::ImageLayout::eUndefined &&
	                                   image_info.tiling == vk::ImageTiling::eOptimal &&
	                                   g_image_recycler.Take(image_info, recycled)) {
		image.image      = recycled.image;
		image.allocation = recycled.allocation;
		image.format     = image_info.format;
		image.image_type = image_info.imageType;
		image.extent     = image_info.extent;
		image.layers     = image_info.arrayLayers;
		image.mip_levels = image_info.mipLevels;
		image.samples    = static_cast<uint32_t>(image_info.samples);
		image.usage      = image_info.usage;
		image.flags      = image_info.flags;
		image.state      = {.layout = vk::ImageLayout::eUndefined};
		image.subresource_states.clear();
		return true;
	}

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	const auto       result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &image.allocation, nullptr));
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	if (ImageRecycler::Enabled()) {
		VmaAllocationInfo allocation_info {};
		vmaGetAllocationInfo(allocator, image.allocation, &allocation_info);
		ImageRecycler::Entry entry;
		entry.format     = image.format;
		entry.image_type = image.image_type;
		entry.extent     = image.extent;
		entry.layers     = image.layers;
		entry.mip_levels = image.mip_levels;
		entry.samples    = image.samples;
		entry.usage      = image.usage;
		entry.flags      = image.flags;
		entry.image      = image.image;
		entry.allocation = image.allocation;
		entry.size       = allocation_info.size;
		std::vector<ImageRecycler::Entry> evicted;
		g_image_recycler.Put(entry, evicted);
		for (const auto& old: evicted) {
			vmaDestroyImage(allocator, old.image, old.allocation);
		}
	} else {
		vmaDestroyImage(allocator, image.image, image.allocation);
	}
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics

// Viewer: loads a character out of the game archives and plays a motion on it.
//
//   g3view <archive.pak> <actor.xact> [motion.xmot]
//
// Left/right drag-free orbit with the arrow keys, W/S to zoom, Space to pause.

#include "genome/pak.h"
#include "render/renderer.h"
#include "render/window.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace
{

// Right-handed look-at with a reversed Z, matching Vulkan's clip space.
std::array<float, 16> lookAt(const std::array<float, 3> &eye, const std::array<float, 3> &target)
{
    std::array<float, 3> forward{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float &value : forward)
        value /= length;

    const std::array<float, 3> worldUp{0.0f, 1.0f, 0.0f};
    std::array<float, 3> right{forward[1] * worldUp[2] - forward[2] * worldUp[1],
                               forward[2] * worldUp[0] - forward[0] * worldUp[2],
                               forward[0] * worldUp[1] - forward[1] * worldUp[0]};
    const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float &value : right)
        value /= rightLength;

    const std::array<float, 3> up{right[1] * forward[2] - right[2] * forward[1],
                                  right[2] * forward[0] - right[0] * forward[2],
                                  right[0] * forward[1] - right[1] * forward[0]};

    return {right[0],
            up[0],
            -forward[0],
            0.0f,
            right[1],
            up[1],
            -forward[1],
            0.0f,
            right[2],
            up[2],
            -forward[2],
            0.0f,
            -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]),
            -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]),
            (forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2]),
            1.0f};
}

std::array<float, 16> perspective(float fovRadians, float aspect, float nearPlane, float farPlane)
{
    const float f = 1.0f / std::tan(fovRadians * 0.5f);
    std::array<float, 16> m{};
    m[0] = f / aspect;
    m[5] = -f; // Vulkan's Y points down in clip space
    m[10] = farPlane / (nearPlane - farPlane);
    m[11] = -1.0f;
    m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
    return m;
}

std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b)
{
    std::array<float, 16> out{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + row] * b[column * 4 + k];
            out[column * 4 + row] = sum;
        }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::puts("usage: g3view <archive.pak> <actor.xact> [motion.xmot]");
        return 2;
    }

    std::string error;
    const auto archive = genome::PakArchive::open(argv[1], &error);
    if (!archive)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    genome::Actor actor;
    if (!genome::loadActor(archive->read(argv[2], &error), actor, &error))
    {
        std::cerr << "actor: " << error << "\n";
        return 1;
    }

    genome::Motion motion;
    const genome::Skeleton skeleton = genome::buildSkeleton(actor);
    if (argc > 3 && !genome::loadMotion(archive->read(argv[3], &error), motion, &error))
    {
        std::cerr << "motion: " << error << "\n";
        return 1;
    }

    std::array<float, 3> min{}, max{};
    actor.computeBounds(min, max);
    const std::array<float, 3> centre{(min[0] + max[0]) * 0.5f, (min[1] + max[1]) * 0.5f, (min[2] + max[2]) * 0.5f};
    const float height = max[1] - min[1];

    std::printf("%s: %zu bones, %zu vertices, %zu triangles, %.0f cm tall\n", argv[2], skeleton.bones.size(),
                actor.vertexCount(), actor.triangleCount(), height);

    render::Window window("Genome runtime", 1280, 720);
    render::Device device;
    if (!device.create(window, &error))
    {
        std::cerr << "vulkan: " << error << "\n";
        return 1;
    }

    render::CharacterRenderer renderer;
    if (!renderer.create(device, actor, &error))
    {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }

    float orbit = 0.6f;
    float distance = height * 1.8f;
    float elevation = 0.15f;
    float time = 0.0f;
    bool paused = false;

    auto previous = std::chrono::steady_clock::now();
    while (window.pump())
    {
        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;

        if (window.keyPressed(render::key::Space))
            paused = !paused;
        if (window.keyDown(render::key::Left))
            orbit -= delta * 1.5f;
        if (window.keyDown(render::key::Right))
            orbit += delta * 1.5f;
        if (window.keyDown(render::key::Up))
            elevation = std::min(elevation + delta, 1.2f);
        if (window.keyDown(render::key::Down))
            elevation = std::max(elevation - delta, -1.2f);
        if (window.keyDown(render::key::W))
            distance = std::max(distance - delta * height, height * 0.4f);
        if (window.keyDown(render::key::S))
            distance = std::min(distance + delta * height, height * 6.0f);
        if (window.keyDown(render::key::Escape))
            break;

        if (!paused && motion.duration > 0.0f)
            time = std::fmod(time + delta, motion.duration);

        if (window.resized() || !device.beginFrame())
        {
            if (!device.recreateSwapchain())
                continue;
            if (!device.beginFrame())
                continue;
        }

        // With no clip the sampler falls back to each bone's rest transform,
        // which is exactly the bind pose.
        renderer.update(device, actor, skeleton, motion, time);

        VkCommandBuffer command = device.commandBuffer();

        VkImageMemoryBarrier toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.image = device.currentColorImage();
        toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toAttachment);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = device.currentColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.09f, 0.10f, 0.12f, 1.0f}};

        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = device.depthView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, device.extent()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(command, &rendering);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(device.extent().width),
                            static_cast<float>(device.extent().height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, device.extent()};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);

        const std::array<float, 3> eye{centre[0] + std::sin(orbit) * distance,
                                       centre[1] + elevation * height,
                                       centre[2] + std::cos(orbit) * distance};
        const std::array<float, 16> view = lookAt(eye, centre);
        const std::array<float, 16> projection =
            perspective(1.0f, static_cast<float>(device.extent().width) / device.extent().height, height * 0.05f,
                        height * 20.0f);
        renderer.draw(device, multiply(projection, view), {0.5f, 0.7f, 0.4f, 0.0f});

        vkCmdEndRendering(command);

        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = device.currentColorImage();
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

        device.endFrame();
    }

    renderer.destroy(device);
    device.destroy();
    return 0;
}

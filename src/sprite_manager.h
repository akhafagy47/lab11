#ifndef SPRITE_MANAGER_H
#define SPRITE_MANAGER_H

#include <array>
#include <cassert>
#include <cstddef>  // offsetof

#include "basic_transform.h"
#include "glad/glad.h"  // must come before any other OpenGL header
#include "texture_atlas.h"
#include "vector_util.h"
#include <SFML/Graphics.hpp>
#include <SFML/OpenGL.hpp>

#define GL_SILENCE_DEPRECATION  // silences macOS OpenGL deprecation warnings

#define glCheck(expr)                                                                              \
    expr;                                                                                          \
    if (int e = glGetError()) {                                                                    \
        std::cerr << "OpenGL error: " << e << " in file " __FILE__ << ", line number " << __LINE__ \
                  << '\n';                                                                         \
        abort();                                                                                   \
    }

using SpriteID = size_t;

class SpriteManager {
public:
    static constexpr size_t VERTICES_PER_SPRITE = 6;

    SpriteManager(const sf::Vector2u& atlasSize, uint padding)
        : mVertices(sf::PrimitiveType::Triangles),
          mNumSprites{0},
          mAtlas(atlasSize, padding),
          mShader(std::filesystem::path{"main.vert"}, std::filesystem::path{"main.frag"}) {
        const GLuint program = mShader.getNativeHandle();

        // Vertex attribute locations
        mPositionAttribLocation = glGetAttribLocation(program, "position");
        mTexCoordsAttribLocation = glGetAttribLocation(program, "texCoords");
        mSpriteNumberAttribLocation = glGetAttribLocation(program, "spriteNumber");

        // Uniform locations
        mTextureUniformLocation = glGetUniformLocation(program, "texture");
        mTransformsUniformLocation = glGetUniformLocation(program, "transforms");
        mNumTransformsUniformLocation = glGetUniformLocation(program, "numTransforms");

        // GPU-side buffers for the vertex array and the sprite numbers
        glCheck(glGenBuffers(1, &mVerticesVBO));
        glCheck(glGenBuffers(1, &mSpriteNumberVBO));

        // GPU-side 1D texture holding every sprite's BasicTransform as one RGBA32F texel
        glCheck(glGenTextures(1, &mTransformsTexture));
        glCheck(glBindTexture(GL_TEXTURE_1D, mTransformsTexture));
        // Float textures generally can't be linearly filtered, and we want exact texels anyway.
        // Also make sure no mipmaps are expected, otherwise the texture is incomplete.
        glCheck(glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glCheck(glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glCheck(glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glCheck(glBindTexture(GL_TEXTURE_1D, 0));
    }

    ~SpriteManager() {
        glDeleteBuffers(1, &mVerticesVBO);
        glDeleteBuffers(1, &mSpriteNumberVBO);
        glDeleteTextures(1, &mTransformsTexture);
    }

    // We own raw OpenGL handles, so copying/moving would lead to double deletion.
    SpriteManager(const SpriteManager&) = delete;
    SpriteManager& operator=(const SpriteManager&) = delete;
    SpriteManager(SpriteManager&&) = delete;
    SpriteManager& operator=(SpriteManager&&) = delete;

    TextureID registerTexture(const std::string& filepath) {
        return mAtlas.registerTexture(filepath);
    }

    SpriteID createSprite(TextureID textureID) {
        const SpriteID id = mNumSprites++;
        for (size_t i = 0; i < VERTICES_PER_SPRITE; ++i) {
            mVertices.append(sf::Vertex{});
            mSpriteNumbers.push_back(static_cast<float>(id));
        }
        mTransforms.emplace_back();
        setTexture(id, textureID);
        return id;
    }

    void setTexture(SpriteID sprite, TextureID newTexture) {
        assert(sprite < mNumSprites);
        const sf::Vector2f size(mAtlas.getSizePixels(newTexture));
        // UVs of the unpadded texture, with origin at the bottom-left
        const sf::FloatRect uv = mAtlas.getBoundsUV(newTexture);

        // Quad corners: bottom-left at the origin, top-right at (width, height) in world units.
        const sf::Vector2f bl{0.f, 0.f}, br{size.x, 0.f}, tr{size.x, size.y}, tl{0.f, size.y};
        const sf::Vector2f uvBL = uv.position;
        const sf::Vector2f uvBR = uv.position + sf::Vector2f{uv.size.x, 0.f};
        const sf::Vector2f uvTR = uv.position + uv.size;
        const sf::Vector2f uvTL = uv.position + sf::Vector2f{0.f, uv.size.y};

        // Two counter-clockwise triangles: (BL, BR, TR) and (BL, TR, TL)
        const std::array<std::pair<sf::Vector2f, sf::Vector2f>, VERTICES_PER_SPRITE> corners = {{
            {bl, uvBL},
            {br, uvBR},
            {tr, uvTR},
            {bl, uvBL},
            {tr, uvTR},
            {tl, uvTL},
        }};

        const size_t base = sprite * VERTICES_PER_SPRITE;
        for (size_t i = 0; i < VERTICES_PER_SPRITE; ++i) {
            mVertices[base + i].position = corners[i].first;
            mVertices[base + i].texCoords = corners[i].second;
        }
    }

    // Batched rendering (part 1): transforms are applied on the CPU, and SFML issues the draw call.
    // Requires the shader to be constructed from main_sfml.vert.
    void draw(sf::RenderTarget& target, sf::RenderStates states) const {
        sf::VertexArray transformed = mVertices;
        for (size_t i = 0; i < transformed.getVertexCount(); ++i) {
            transformed[i].position =
                mTransforms[i / VERTICES_PER_SPRITE].applyPoint(transformed[i].position);
        }
        states.texture = &mAtlas.getTexture();
        states.shader = &mShader;
        target.draw(transformed, states);
    }

    // Batched rendering (part 2): transforms are applied on the GPU in main.vert.
    void drawGL() {
        if (mNumSprites == 0) {
            return;
        }
        glCheck(glUseProgram(mShader.getNativeHandle()));

        // --- Vertex positions + UVs (interleaved sf::Vertex data) ---
        const auto vertexCount = static_cast<GLsizei>(mVertices.getVertexCount());
        glCheck(glBindBuffer(GL_ARRAY_BUFFER, mVerticesVBO));
        glCheck(glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(vertexCount * sizeof(sf::Vertex)),
                             &mVertices[0], GL_DYNAMIC_DRAW));
        if (mPositionAttribLocation != -1) {
            glCheck(glVertexAttribPointer(
                static_cast<GLuint>(mPositionAttribLocation), 2, GL_FLOAT, GL_FALSE,
                sizeof(sf::Vertex), reinterpret_cast<const void*>(offsetof(sf::Vertex, position))));
            glCheck(glEnableVertexAttribArray(static_cast<GLuint>(mPositionAttribLocation)));
        }
        if (mTexCoordsAttribLocation != -1) {
            glCheck(glVertexAttribPointer(
                static_cast<GLuint>(mTexCoordsAttribLocation), 2, GL_FLOAT, GL_FALSE,
                sizeof(sf::Vertex), reinterpret_cast<const void*>(offsetof(sf::Vertex, texCoords))));
            glCheck(glEnableVertexAttribArray(static_cast<GLuint>(mTexCoordsAttribLocation)));
        }

        // --- Sprite numbers ---
        glCheck(glBindBuffer(GL_ARRAY_BUFFER, mSpriteNumberVBO));
        glCheck(glBufferData(GL_ARRAY_BUFFER,
                             static_cast<GLsizeiptr>(mSpriteNumbers.size() * sizeof(float)),
                             mSpriteNumbers.data(), GL_DYNAMIC_DRAW));
        if (mSpriteNumberAttribLocation != -1) {
            glCheck(glVertexAttribPointer(static_cast<GLuint>(mSpriteNumberAttribLocation), 1,
                                          GL_FLOAT, GL_FALSE, sizeof(float), nullptr));
            glCheck(glEnableVertexAttribArray(static_cast<GLuint>(mSpriteNumberAttribLocation)));
        }
        glCheck(glBindBuffer(GL_ARRAY_BUFFER, 0));

        // --- Transforms texture on texture unit 1 ---
        static_assert(sizeof(BasicTransform) == 4 * sizeof(float));
        glCheck(glActiveTexture(GL_TEXTURE1));
        glCheck(glBindTexture(GL_TEXTURE_1D, mTransformsTexture));
        glCheck(glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F_ARB,
                             static_cast<GLsizei>(mTransforms.size()), 0, GL_RGBA, GL_FLOAT,
                             mTransforms.data()));
        glCheck(glUniform1i(mTransformsUniformLocation, 1));

        // --- Atlas texture on texture unit 0 ---
        glCheck(glActiveTexture(GL_TEXTURE0));
        glCheck(glBindTexture(GL_TEXTURE_2D, mAtlas.getTexture().getNativeHandle()));
        glCheck(glUniform1i(mTextureUniformLocation, 0));

        // --- Non-texture uniforms ---
        glCheck(glUniform1i(mNumTransformsUniformLocation, static_cast<GLint>(mTransforms.size())));

        // --- Draw everything in a single call ---
        glCheck(glDrawArrays(GL_TRIANGLES, 0, vertexCount));

        // Leave the attribute state clean for anyone drawing after us
        if (mPositionAttribLocation != -1) {
            glDisableVertexAttribArray(static_cast<GLuint>(mPositionAttribLocation));
        }
        if (mTexCoordsAttribLocation != -1) {
            glDisableVertexAttribArray(static_cast<GLuint>(mTexCoordsAttribLocation));
        }
        if (mSpriteNumberAttribLocation != -1) {
            glDisableVertexAttribArray(static_cast<GLuint>(mSpriteNumberAttribLocation));
        }
        glUseProgram(0);
    }

    BasicTransform& getTransform(SpriteID id) {
        assert(id < mTransforms.size());
        return mTransforms[id];
    }

    const TextureAtlas& getAtlas() const { return mAtlas; }

private:
    sf::VertexArray mVertices;
    std::vector<BasicTransform> mTransforms;
    size_t mNumSprites;
    TextureAtlas mAtlas;
    mutable sf::Shader mShader;

    // Use the below only in Batched rendering (part 2):
    std::vector<float> mSpriteNumbers;

    GLint mTextureUniformLocation;
    GLint mTransformsUniformLocation;
    GLint mNumTransformsUniformLocation;

    GLint mPositionAttribLocation;
    GLint mTexCoordsAttribLocation;
    GLint mSpriteNumberAttribLocation;

    GLuint mVerticesVBO;
    GLuint mSpriteNumberVBO;
    GLuint mTransformsTexture;
};

#endif  // SPRITE_MANAGER_H

#pragma once

#include "../CommonGL/graphicsDeviceGL.h"

class GraphicsShadersGL_2_0;
class VertexBufferGL;
class IndexBufferGL;

class GraphicsDeviceGL_2_0 : public GraphicsDeviceGL
{
    public:
        GraphicsDeviceGL_2_0(GraphicsDevicePlatform* platform);
        virtual ~GraphicsDeviceGL_2_0();

		bool init(int w, int h, int& vw, int& vh);

		void setShader(ShaderID shader);

		void setShaderResource(TextureHandle handle, u32 nameHash);
		void drawQuad(const Quad& quad);
		void drawFullscreenQuad(TextureGL* tex);

		void drawVirtualScreen();
		void setVirtualViewport(bool reset, int x, int y, int w, int h);
    protected:
		GraphicsShadersGL_2_0* m_shaders;
		VertexBufferGL* m_quadVB;
		IndexBufferGL*  m_quadIB;

		ShaderGL* m_curShader;
		u32 m_curShaderID;
};

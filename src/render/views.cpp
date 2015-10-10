#include "views.h"
#include "load.h"
#include "asset/shader.h"

namespace VI
{

View::View()
	: mesh(AssetNull), shader(AssetNull), texture(AssetNull), offset(Mat4::identity), color(1, 1, 1, 1)
{
}

void View::draw(const RenderParams& params)
{
	Loader::mesh(mesh);
	Loader::shader(shader);
	Loader::texture(texture);

	RenderSync* sync = params.sync;
	sync->write(RenderOp_Mesh);
	sync->write(&mesh);
	sync->write(&shader);

	Mat4 m;
	get<Transform>()->mat(&m);
	m = offset * m;
	Mat4 mvp = m * params.view_projection;

	sync->write<int>(texture == AssetNull ? 4 : 5); // Uniform count

	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType_Mat4);
	sync->write<int>(1);
	sync->write(&mvp);

	sync->write(Asset::Uniform::m);
	sync->write(RenderDataType_Mat4);
	sync->write<int>(1);
	sync->write(&m);

	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType_Vec4);
	sync->write<int>(1);
	sync->write(&color);

	sync->write(Asset::Uniform::light_position);
	sync->write(RenderDataType_Vec3);
	sync->write<int>(1);
	sync->write(&params.camera->pos);

	if (texture != AssetNull)
	{
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType_Texture);
		sync->write<int>(1);
		sync->write(&texture);
		sync->write<RenderTextureType>(RenderTexture2D);
	}
}

void View::awake()
{
	Mesh* m = Loader::mesh(mesh);
	color = m->color;
	Loader::shader(shader);
	Loader::texture(texture);
}

AssetID Skybox::texture = AssetNull;
AssetID Skybox::mesh = AssetNull;
AssetID Skybox::shader = AssetNull;
Vec4 Skybox::color = Vec4(1, 1, 1, 1);

void Skybox::set(const Vec4& c, const AssetID& s, const AssetID& m, const AssetID& t)
{
	color = c;

	if (shader != AssetNull && shader != s)
		Loader::shader_free(shader);
	shader = s;
	Loader::shader(s);

	if (mesh != AssetNull && mesh != m)
		Loader::mesh_free(mesh);
	mesh = m;
	Loader::mesh(m);

	if (texture != AssetNull && texture != t)
		Loader::texture_free(texture);
	texture = t;
	Loader::texture(t);
}

void Skybox::draw(const RenderParams& p)
{
	if (shader == AssetNull || mesh == AssetNull)
		return;

	Loader::shader(shader);
	Loader::mesh(mesh);
	Loader::texture(texture);

	RenderSync* sync = p.sync;

	sync->write(RenderOp_DepthMask);
	sync->write<bool>(false);

	sync->write(RenderOp_Mesh);
	sync->write(mesh);
	sync->write(shader);

	sync->write<int>(texture == AssetNull ? 2 : 3); // Uniform count

	Mat4 mvp = p.view;
	mvp.translation(Vec3::zero);
	mvp = mvp * p.camera->projection;

	sync->write(Asset::Uniform::mvp);
	sync->write(RenderDataType_Mat4);
	sync->write<int>(1);
	sync->write(&mvp);

	sync->write(Asset::Uniform::diffuse_color);
	sync->write(RenderDataType_Vec4);
	sync->write<int>(1);
	sync->write(&color);

	if (texture != AssetNull)
	{
		sync->write(Asset::Uniform::diffuse_map);
		sync->write(RenderDataType_Texture);
		sync->write<int>(1);
		sync->write(&texture);
		sync->write<RenderTextureType>(RenderTexture2D);
	}

	sync->write(RenderOp_DepthMask);
	sync->write<bool>(true);
}

}

#include "stdafx.h"
#include "mu_environment.h"
#include "mu_state.h"
#include "mu_camera.h"
#include "mu_modelrenderer.h"
#include "mu_bboxrenderer.h"
#include "mu_renderstate.h"
#include "mu_threadsmanager.h"
#include <algorithm>
#include <execution>

enum class NThreadMode {
	Single,
	Multi,
	MultiSTL,
};

#define RENDER_BBOX (0)
static NThreadMode ThreadMode = NThreadMode::Multi;

const mu_boolean NEnvironment::Initialize()
{
	Particles.reset(new (std::nothrow) NParticles());
	if (!Particles || Particles->Initialize() == false)
	{
		return false;
	}

	Joints.reset(new (std::nothrow) NJoints());
	if (!Joints || Joints->Initialize() == false)
	{
		return false;
	}

	ObjectsRange.resize(MUThreadsManager::GetThreadsCount());

	return true;
}

void NEnvironment::Destroy()
{
	if (Particles)
	{
		Particles->Destroy();
		Particles.reset();
	}

	if (Joints)
	{
		Joints->Destroy();
		Joints.reset();
	}
}

void NEnvironment::Reset()
{
	const auto updateCount = MUState::GetUpdateCount();

	if (updateCount > 0)
	{
		Terrain->Reset();
	}
}

void NEnvironment::Update()
{
	const auto updateCount = MUState::GetUpdateCount();
	const auto updateTime = MUState::GetUpdateTime();

	if (updateCount > 0)
	{
		Terrain->Update();
	}

	const auto frustum = MURenderState::GetCamera()->GetFrustum();
	const NThreadMode threadMode = ThreadMode;

	// Animate
	if (threadMode == NThreadMode::MultiSTL)
	{
		const auto environment = this;
		const auto view = Objects.view<
			NEntity::Renderable,
			NEntity::Attachment,
			NEntity::Light,
			NEntity::RenderState,
			NEntity::Skeleton,
			NEntity::Position,
			NEntity::Animation,
			NEntity::BoundingBox
		>();
		std::for_each(
			std::execution::par_unseq,
			view.begin(), view.end(),
			[&view, environment, frustum, updateTime](const entt::entity entity) {
				auto [attachment, light, renderState, skeleton, position, animation, boundingBox] = view.get<
					NEntity::Attachment,
					NEntity::Light,
					NEntity::RenderState,
					NEntity::Skeleton,
					NEntity::Position,
					NEntity::Animation,
					NEntity::BoundingBox
				>(entity);

				skeleton.Instance.SetParent(
					position.Angle,
					position.Position,
					position.Scale
				);
				skeleton.Instance.PlayAnimation(attachment.Model, animation.CurrentAction, animation.PriorAction, animation.CurrentFrame, animation.PriorFrame, attachment.Model->GetPlaySpeed() * updateTime);

				NCompressedMatrix viewModel;
				viewModel.Set(
					position.Angle,
					position.Position,
					position.Scale
				);

				const auto model = attachment.Model;
				if (model->HasMeshes() && model->HasGlobalBBox())
				{
					const auto &bbox = model->GetGlobalBBox();
					boundingBox.Min = Transform(bbox.Min, viewModel);
					boundingBox.Max = Transform(bbox.Max, viewModel);
				}
				else
				{
					boundingBox.Min = Transform(boundingBox.Min, viewModel);
					boundingBox.Max = Transform(boundingBox.Max, viewModel);
				}
				boundingBox.Order();

				renderState.Flags.Visible = frustum->IsBoxVisible(boundingBox.Min, boundingBox.Max);

				if (!renderState.Flags.Visible) return;

				environment->CalculateLight(position, light, renderState);
				skeleton.Instance.Animate(
					attachment.Model,
					{
						.Action = animation.CurrentAction,
						.Frame = animation.CurrentFrame,
					},
					{
						.Action = animation.PriorAction,
						.Frame = animation.PriorFrame,
					},
					glm::vec3(0.0f, 0.0f, 0.0f)
					);
				skeleton.SkeletonOffset = skeleton.Instance.Upload();
			}
		);
	}
	else if (threadMode == NThreadMode::Multi)
	{
		const auto view = Objects.view<
			NEntity::Renderable
		>();
		const mu_uint32 entitiesCount = static_cast<mu_uint32>(view.size());
		auto &registry = Objects;
		auto &threadsRange = ObjectsRange;

		TThreading::SplitLoopIndex(entitiesCount, threadsRange);
		MUThreadsManager::Run(
			[&registry, &view, &threadsRange, updateTime](const mu_uint32 threadIndex) -> void {
				const auto &range = threadsRange[threadIndex];
				for (auto iter = view.begin() + range.start, last = view.begin() + range.end; iter != last; ++iter)
				{
					const auto &entity = *iter;
					auto [attachment, skeleton, position, animation] = registry.get<
						NEntity::Attachment,
						NEntity::Skeleton,
						NEntity::Position,
						NEntity::Animation
					>(entity);

					skeleton.Instance.SetParent(
						position.Angle,
						position.Position,
						position.Scale
					);
					skeleton.Instance.PlayAnimation(attachment.Model, animation.CurrentAction, animation.PriorAction, animation.CurrentFrame, animation.PriorFrame, attachment.Model->GetPlaySpeed() * updateTime);
				}
			}
		);

		TThreading::SplitLoopIndex(entitiesCount, threadsRange);
		MUThreadsManager::Run(
			[&registry, &view, &threadsRange, frustum](const mu_uint32 threadIndex) -> void {
				const auto &range = threadsRange[threadIndex];
				for (auto iter = view.begin() + range.start, last = view.begin() + range.end; iter != last; ++iter)
				{
					const auto &entity = *iter;
					auto [attachment, renderState, boundingBox, position] = registry.get<
						NEntity::Attachment,
						NEntity::RenderState,
						NEntity::BoundingBox,
						NEntity::Position
					>(entity);

					NCompressedMatrix viewModel;
					viewModel.Set(
						position.Angle,
						position.Position,
						position.Scale
					);

					const auto model = attachment.Model;
					if (model->HasMeshes() && model->HasGlobalBBox())
					{
						const auto &bbox = model->GetGlobalBBox();
						boundingBox.Min = Transform(bbox.Min, viewModel);
						boundingBox.Max = Transform(bbox.Max, viewModel);
					}
					else
					{
						boundingBox.Min = Transform(boundingBox.Min, viewModel);
						boundingBox.Max = Transform(boundingBox.Max, viewModel);
					}
					boundingBox.Order();

					renderState.Flags.Visible = frustum->IsBoxVisible(boundingBox.Min, boundingBox.Max);
				}
			}
		);

		const auto environment = this;
		TThreading::SplitLoopIndex(entitiesCount, threadsRange);
		MUThreadsManager::Run(
			[environment, &registry, &view, &threadsRange, frustum](const mu_uint32 threadIndex) -> void {
				const auto &range = threadsRange[threadIndex];
				for (auto iter = view.begin() + range.start, last = view.begin() + range.end; iter != last; ++iter)
				{
					const auto &entity = *iter;
					auto [attachment, light, renderState, skeleton, position, animation] = registry.get<
						NEntity::Attachment,
						NEntity::Light,
						NEntity::RenderState,
						NEntity::Skeleton,
						NEntity::Position,
						NEntity::Animation
					>(entity);

					if (!renderState.Flags.Visible) continue;

					environment->CalculateLight(position, light, renderState);
					skeleton.Instance.Animate(
						attachment.Model,
						{
							.Action = animation.CurrentAction,
							.Frame = animation.CurrentFrame,
						},
						{
							.Action = animation.PriorAction,
							.Frame = animation.PriorFrame,
						},
						glm::vec3(0.0f, 0.0f, 0.0f)
						);
					skeleton.SkeletonOffset = skeleton.Instance.Upload();
				}
			}
		);
	}
	else
	{
		// Animate
		{
			const auto view = Objects.view<
				NEntity::Renderable,
				NEntity::Attachment,
				NEntity::Skeleton,
				NEntity::Position,
				NEntity::Animation
			>();
			for (auto [entity, attachment, skeleton, position, animation] : view.each())
			{
				skeleton.Instance.SetParent(
					position.Angle,
					position.Position,
					position.Scale
				);
				skeleton.Instance.PlayAnimation(attachment.Model, animation.CurrentAction, animation.PriorAction, animation.CurrentFrame, animation.PriorFrame, attachment.Model->GetPlaySpeed() * updateTime);
			}
		}

		// Culling
		{
			const auto view = Objects.view<
				NEntity::Renderable,
				NEntity::Attachment,
				NEntity::RenderState,
				NEntity::BoundingBox,
				NEntity::Position
			>();
			for (auto [entity, attachment, renderState, boundingBox, position] : view.each())
			{
				NCompressedMatrix viewModel;
				viewModel.Set(
					position.Angle,
					position.Position,
					position.Scale
				);

				const auto model = attachment.Model;
				if (model->HasMeshes() && model->HasGlobalBBox())
				{
					const auto &bbox = model->GetGlobalBBox();
					boundingBox.Min = Transform(bbox.Min, viewModel);
					boundingBox.Max = Transform(bbox.Max, viewModel);
				}
				else
				{
					boundingBox.Min = Transform(boundingBox.Min, viewModel);
					boundingBox.Max = Transform(boundingBox.Max, viewModel);
				}
				boundingBox.Order();

				renderState.Flags.Visible = frustum->IsBoxVisible(boundingBox.Min, boundingBox.Max);
			}
		}

		// Prepare to Render
		{
			const auto view = Objects.view<
				NEntity::Renderable,
				NEntity::Attachment,
				NEntity::Light,
				NEntity::RenderState,
				NEntity::Skeleton,
				NEntity::Position,
				NEntity::Animation
			>();
			for (auto [entity, attachment, light, renderState, skeleton, position, animation] : view.each())
			{
				if (!renderState.Flags.Visible) continue;

				CalculateLight(position, light, renderState);
				skeleton.Instance.Animate(
					attachment.Model,
					{
						.Action = animation.CurrentAction,
						.Frame = animation.CurrentFrame,
					},
				{
					.Action = animation.PriorAction,
					.Frame = animation.PriorFrame,
				},
				glm::vec3(0.0f, 0.0f, 0.0f)
				);
				skeleton.SkeletonOffset = skeleton.Instance.Upload();
			}
		}
	}

	Particles->Update(updateCount);
	Particles->Propagate();

	Joints->Update(updateCount);
	Joints->Propagate();
}

void NEnvironment::Render()
{
	Terrain->ConfigureUniforms();
	Terrain->Render();

	const auto objectsView = Objects.view<NEntity::Renderable, NEntity::Attachment, NEntity::RenderState, NEntity::Skeleton>();
	for (auto [entity, attachment, renderState, skeleton] : objectsView.each())
	{
		if (!renderState.Flags.Visible) continue;
		if (skeleton.SkeletonOffset == NInvalidUInt32) continue;
		const NRenderConfig config = {
			.BoneOffset = skeleton.SkeletonOffset,
			.BodyOrigin = glm::vec3(0.0f, 0.0f, 0.0f),
			.BodyScale = 1.0f,
			.EnableLight = renderState.Flags.LightEnable,
			.BodyLight = renderState.BodyLight,
		};
		MUModelRenderer::RenderBody(skeleton.Instance, attachment.Model, config);
	}

#if RENDER_BBOX
	const auto bboxView = Objects.view<NEntity::Renderable, NEntity::RenderState, NEntity::BoundingBox>();
	for (auto [entity, renderState, boundingBox] : bboxView.each())
	{
		if (!renderState.Flags.Visible) continue;
		MUBBoxRenderer::Render(boundingBox);
	}
#endif

	Particles->Render();
	Joints->Render();
}

void NEnvironment::CalculateLight(
	const NEntity::Position &position,
	const NEntity::Light &light,
	NEntity::RenderState &renderState
)
{
	switch (light.Mode)
	{
	case EntityLightMode::Terrain:
		{
			const auto &terrain = light.Settings.Terrain;
			const glm::vec3 terrainLight = (
				terrain.PrimaryLight
				? Terrain->CalculatePrimaryLight(position.Position[0], position.Position[1])
				: Terrain->CalculateBackLight(position.Position[0], position.Position[1])
			);
			renderState.BodyLight = glm::vec4(terrain.Color + terrainLight, 1.0f);
		}
		break;

	case EntityLightMode::Fixed:
		{
			const auto &fixed = light.Settings.Fixed;
			renderState.BodyLight = glm::vec4(fixed.Color, 1.0f);
		}
		break;

	case EntityLightMode::SinWorldTime:
		{
			const auto &worldTime = light.Settings.WorldTime;
			mu_float luminosity = glm::sin(MUState::GetWorldTime() * worldTime.TimeMultiplier) * worldTime.Multiplier + worldTime.Add;
			renderState.BodyLight = glm::vec4(luminosity, luminosity, luminosity, 1.0f);
		}
		break;
	}
}
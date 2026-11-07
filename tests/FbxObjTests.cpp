#include "GltfTestUtils.hpp"
#include "TestUtils.hpp"

#include "assets/AssetImport.hpp"
#include "assets/AssetLibrary.hpp"
#include "assets/FbxImporter.hpp"
#include "assets/GltfImporter.hpp"
#include "scene/ModelInstance.hpp"
#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <fstream>

using namespace lumora;

namespace {

void writePng(const std::filesystem::path& file, unsigned char r, unsigned char g, unsigned char b) {
    std::filesystem::create_directories(file.parent_path());
    const auto png = test::solidPng(4, 4, r, g, b);
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

int nodeNamed(const ImportedModel& model, const std::string& name) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

glm::mat4 worldOf(const ImportedModel& model, int node) {
    glm::mat4 m(1.0f);
    for (int n = node; n >= 0; n = model.nodes[static_cast<std::size_t>(n)].parent) {
        const ImportedNode& in = model.nodes[static_cast<std::size_t>(n)];
        m = glm::translate(glm::mat4(1.0f), in.translation) * glm::mat4_cast(in.rotation) * glm::scale(glm::mat4(1.0f), in.scale) * m;
    }
    return m;
}

/// Two quads of a "Box" object with two materials from a material library: a half-transparent
/// red one, and one with a base colour map in a sub-folder.
void writeObj(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "scene.obj") << R"(# test
mtllib scene.mtl
o Box
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 2 0 0
v 2 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
usemtl Red
f 1/1/1 2/2/1 3/3/1 4/4/1
usemtl Leafy
f 2/1/1 5/2/1 6/3/1 3/4/1
)";
    std::ofstream(dir / "scene.mtl") << R"(newmtl Red
Kd 0.9 0.1 0.05
d 0.5
newmtl Leafy
Kd 1 1 1
map_Kd textures/leaf.png
)";
    writePng(dir / "textures" / "leaf.png", 20, 200, 40);
}

/// An ASCII FBX 7.4 in centimetres with Z up (as 3ds Max writes it): a "Floor" quad 50 cm above
/// the ground with two materials; the second's map was saved under a path on another machine
/// and travels next to the model.
void writeFbx(const std::filesystem::path& file) {
    std::ofstream(file) << R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
	Creator: "Lumora test"
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",2
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",1
		P: "FrontAxisSign", "int", "Integer", "",-1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Floor", "Mesh" {
		Vertices: *18 {
			a: 0,0,0,100,0,0,100,100,0,0,100,0,200,0,0,200,100,0
		}
		PolygonVertexIndex: *8 {
			a: 0,1,2,-4,1,4,5,-3
		}
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *24 {
				a: 0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "UVMap"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *8 {
				a: 0,1,2,3,0,1,2,3
			}
		}
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygon"
			ReferenceInformationType: "IndexToDirect"
			Materials: *2 {
				a: 0,1
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	Model: 2000, "Model::Floor", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,0,50
		}
		Shading: T
		Culling: "CullingOff"
	}
	Material: 3000, "Material::Paint", "" {
		Version: 102
		ShadingModel: "phong"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.8,0.2,0.1
			P: "DiffuseFactor", "Number", "", "A",1
		}
	}
	Material: 3001, "Material::Grass", "" {
		Version: 102
		ShadingModel: "phong"
		MultiLayer: 0
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.5,0.5,0.5
		}
	}
	Texture: 4000, "Texture::Grass", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::Grass"
		FileName: "C:/Users/artist/Projects/maps/grass.png"
		RelativeFilename: "..\..\maps\grass.png"
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
	C: "OO",3000,2000
	C: "OO",3001,2000
	C: "OP",4000,3001, "DiffuseColor"
}
)";
}

/// An ASCII FBX in metres, Y up: a "Bone" (limb) moving 2 m along X in one second with a "Tip"
/// 1 m above it, and a triangle weighted to them: the top corner 25% Bone, 75% Tip, the rest
/// fully Bone. The bone's bind pose is 1 m up.
void writeSkinnedFbx(const std::filesystem::path& file) {
    std::ofstream(file) << R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Tri", "Mesh" {
		Vertices: *9 {
			a: 0,0,0,1,0,0,0,1,0
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::Body", "Mesh" {
		Version: 232
	}
	Model: 2001, "Model::Bone", "LimbNode" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,1,0
		}
	}
	Model: 2002, "Model::Tip", "LimbNode" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",0,1,0
		}
	}
	NodeAttribute: 2101, "NodeAttribute::Bone", "LimbNode" {
		TypeFlags: "Skeleton"
	}
	Deformer: 5000, "Deformer::Skin", "Skin" {
		Version: 101
		Link_DeformAcuracy: 50
	}
	Deformer: 5001, "SubDeformer::Cluster Bone", "Cluster" {
		Version: 100
		Indexes: *3 {
			a: 0,1,2
		}
		Weights: *3 {
			a: 1,1,0.25
		}
		Transform: *16 {
			a: 1,0,0,0,0,1,0,0,0,0,1,0,0,-1,0,1
		}
		TransformLink: *16 {
			a: 1,0,0,0,0,1,0,0,0,0,1,0,0,1,0,1
		}
	}
	Deformer: 5002, "SubDeformer::Cluster Tip", "Cluster" {
		Version: 100
		Indexes: *1 {
			a: 2
		}
		Weights: *1 {
			a: 0.75
		}
		Transform: *16 {
			a: 1,0,0,0,0,1,0,0,0,0,1,0,0,-2,0,1
		}
		TransformLink: *16 {
			a: 1,0,0,0,0,1,0,0,0,0,1,0,0,2,0,1
		}
	}
	AnimationStack: 6000, "AnimStack::Slide", "" {
		Properties70:  {
			P: "LocalStop", "KTime", "Time", "",46186158000
		}
	}
	AnimationLayer: 6001, "AnimLayer::Base", "" {
	}
	AnimationCurveNode: 6002, "AnimCurveNode::T", "" {
		Properties70:  {
			P: "d|X", "Number", "", "A",0
			P: "d|Y", "Number", "", "A",1
			P: "d|Z", "Number", "", "A",0
		}
	}
	AnimationCurve: 6003, "AnimCurve::", "" {
		Default: 0
		KeyVer: 4009
		KeyTime: *2 {
			a: 0,46186158000
		}
		KeyValueFloat: *2 {
			a: 0,2
		}
		KeyAttrFlags: *1 {
			a: 260
		}
		KeyAttrDataFloat: *4 {
			a: 0,0,0,0
		}
		KeyAttrRefCount: *1 {
			a: 2
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",2001,0
	C: "OO",2101,2001
	C: "OO",1000,2000
	C: "OO",5000,1000
	C: "OO",5001,5000
	C: "OO",2001,5001
	C: "OO",2002,2001
	C: "OO",5002,5000
	C: "OO",2002,5002
	C: "OO",6001,6000
	C: "OO",6002,6001
	C: "OP",6002,2001, "Lcl Translation"
	C: "OP",6003,6002, "d|X"
}
)";
}

} // namespace

TEST(ObjImport, MaterialsTexturesAndUvs) {
    test::TempDir dir;
    writeObj(dir.path() / "src");
    EXPECT_TRUE(isModelFile("a/B.OBJ"));
    EXPECT_TRUE(isModelFile("b.fbx"));
    auto model = importModel(dir.path() / "src" / "scene.obj");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    for (const std::string& w : model->warnings) ADD_FAILURE() << "warning: " << w;
    ASSERT_EQ(model->materials.size(), 2u);
    const ImportedMaterial& red = model->materials[0].name == "Red" ? model->materials[0] : model->materials[1];
    const ImportedMaterial& leafy = model->materials[0].name == "Red" ? model->materials[1] : model->materials[0];
    EXPECT_NEAR(red.baseColor.r, 0.9f, 1e-4f);
    EXPECT_NEAR(red.baseColor.a, 0.5f, 1e-4f);
    EXPECT_TRUE(red.translucent) << "d 0.5 is see-through";
    ASSERT_GE(leafy.baseColorTexture, 0);
    EXPECT_FALSE(model->images[static_cast<std::size_t>(leafy.baseColorTexture)].encoded.empty());
    EXPECT_EQ(model->images[static_cast<std::size_t>(leafy.baseColorTexture)].mimeType, "image/png");

    // One object, split per material: two parts of one mesh on one node.
    ASSERT_EQ(model->meshParts.size(), 1u);
    ASSERT_EQ(model->meshParts[0].size(), 2u);
    std::size_t triangles = 0;
    for (const ImportedMesh& mesh : model->meshes) {
        triangles += mesh.data.indices.size() / 3;
        EXPECT_EQ(mesh.data.vertices.size(), 4u) << "corners shared within a quad";
        for (const Vertex& v : mesh.data.vertices) {
            EXPECT_NEAR(v.normal.z, 1.0f, 1e-5f);
            if (v.position.x == 0.0f && v.position.y == 0.0f) {
                EXPECT_FLOAT_EQ(v.uv.y, 1.0f) << "OBJ v points up";
            }
        }
    }
    EXPECT_EQ(triangles, 4u);
    EXPECT_FLOAT_EQ(model->unitScale, 1.0f);
    EXPECT_NEAR(model->boundsMax.x, 2.0f, 1e-5f);

    auto deps = modelDependencies(dir.path() / "src" / "scene.obj");
    ASSERT_TRUE(deps.has_value()) << deps.error().message;
    EXPECT_NE(std::ranges::find(*deps, std::filesystem::path("scene.mtl")), deps->end());
    EXPECT_NE(std::ranges::find(*deps, std::filesystem::path("textures/leaf.png")), deps->end());

    // Importing copies the library and the map along, and the copy loads the same.
    auto copied = importModelFile(dir.path() / "src" / "scene.obj", dir.path() / "Content" / "Models");
    ASSERT_TRUE(copied.has_value()) << copied.error().message;
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "Content" / "Models" / "scene.mtl"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "Content" / "Models" / "textures" / "leaf.png"));
    AssetLibrary library(dir.path());
    auto texture = library.texture(std::format("Content/Models/scene.obj#tex{}", leafy.baseColorTexture));
    ASSERT_TRUE(texture.has_value()) << texture.error().message;
    EXPECT_EQ((*texture)->rgba[1], 200);
}

TEST(FbxImport, CentimetresZUpMaterialsAndTexturesNextToTheModel) {
    test::TempDir dir;
    writeFbx(dir.path() / "floor.fbx");
    writePng(dir.path() / "grass.png", 30, 160, 20);
    auto model = importModel(dir.path() / "floor.fbx");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    EXPECT_NEAR(model->unitScale, 0.01f, 1e-6f) << "centimetres";
    ASSERT_EQ(model->materials.size(), 2u);
    EXPECT_EQ(model->materials[0].name, "Paint");
    EXPECT_NEAR(model->materials[0].baseColor.r, 0.8f, 1e-4f);
    EXPECT_NEAR(model->materials[0].baseColor.g, 0.2f, 1e-4f);
    EXPECT_EQ(model->materials[0].baseColorTexture, -1);
    ASSERT_GE(model->materials[1].baseColorTexture, 0) << "found by its name next to the model";
    EXPECT_FLOAT_EQ(model->materials[1].baseColor.r, 1.0f) << "the map replaces the colour";

    const int floor = nodeNamed(*model, "Floor");
    ASSERT_GE(floor, 0);
    ASSERT_GE(model->nodes[static_cast<std::size_t>(floor)].mesh, 0);
    EXPECT_EQ(model->meshParts[static_cast<std::size_t>(model->nodes[static_cast<std::size_t>(floor)].mesh)].size(), 2u);
    // Z up became Y up: 50 cm up is +Y, the quads lie flat, the file's +Y (away) is -Z.
    const glm::vec3 origin(worldOf(*model, floor) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(origin.y, 50.0f, 1e-3f);
    EXPECT_NEAR(model->boundsMin.y, 50.0f, 1e-3f);
    EXPECT_NEAR(model->boundsMax.y, 50.0f, 1e-3f);
    EXPECT_NEAR(model->boundsMax.x, 200.0f, 1e-3f);
    EXPECT_NEAR(model->boundsMin.z, -100.0f, 1e-3f);
    EXPECT_NEAR(model->boundsMax.z, 0.0f, 1e-3f);
    const glm::vec3 up = glm::normalize(glm::vec3(worldOf(*model, floor) *
                                                  glm::vec4(model->meshes[static_cast<std::size_t>(
                                                                model->meshParts[static_cast<std::size_t>(
                                                                    model->nodes[static_cast<std::size_t>(floor)].mesh)][0])]
                                                                .data.vertices[0]
                                                                .normal,
                                                            0.0f)));
    EXPECT_NEAR(up.y, 1.0f, 1e-4f);

    auto deps = modelDependencies(dir.path() / "floor.fbx");
    ASSERT_TRUE(deps.has_value()) << deps.error().message;
    ASSERT_EQ(deps->size(), 1u);
    EXPECT_EQ((*deps)[0], std::filesystem::path("grass.png"));
    const auto refs = fbxReferences(dir.path() / "floor.fbx");
    EXPECT_NE(std::ranges::find(refs, std::filesystem::path("grass.png")), refs.end()) << "outside the folder: the bare name";

    // In a scene: the root takes the units.
    Scene scene;
    const entt::entity root = instantiateModel(scene, *model, "floor.fbx", "Floor");
    EXPECT_NEAR(scene.registry().get<TransformComponent>(root).scale.x, 0.01f, 1e-6f);
}

TEST(FbxImport, SkinAndAnimation) {
    test::TempDir dir;
    writeSkinnedFbx(dir.path() / "rig.fbx");
    auto model = importModel(dir.path() / "rig.fbx");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    EXPECT_FLOAT_EQ(model->unitScale, 1.0f) << "metres";
    const int bone = nodeNamed(*model, "Bone");
    const int body = nodeNamed(*model, "Body");
    ASSERT_GE(bone, 0);
    ASSERT_GE(body, 0);
    EXPECT_EQ(model->nodes[static_cast<std::size_t>(bone)].parent, -1) << "identity root left out";
    ASSERT_EQ(model->skins.size(), 1u);
    const int tip = nodeNamed(*model, "Tip");
    ASSERT_GE(tip, 0);
    EXPECT_EQ(model->nodes[static_cast<std::size_t>(tip)].parent, bone);
    ASSERT_EQ(model->skins[0].joints.size(), 2u);
    EXPECT_EQ(model->skins[0].joints[0], bone);
    EXPECT_EQ(model->skins[0].joints[1], tip);
    EXPECT_EQ(model->nodes[static_cast<std::size_t>(body)].skin, 0);
    const glm::vec3 unbound(model->skins[0].inverseBind[0] * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    EXPECT_NEAR(glm::length(unbound), 0.0f, 1e-5f) << "bind pose 1 m up";
    const ImportedMesh& mesh = model->meshes[static_cast<std::size_t>(
        model->meshParts[static_cast<std::size_t>(model->nodes[static_cast<std::size_t>(body)].mesh)][0])];
    ASSERT_EQ(mesh.data.skin.size(), mesh.data.vertices.size());
    for (std::size_t v = 0; v < mesh.data.vertices.size(); ++v) {
        const SkinVertex& s = mesh.data.skin[v];
        if (mesh.data.vertices[v].position.y > 0.5f) {  // the top corner: the larger weight first
            EXPECT_EQ(s.joints[0], 1);
            EXPECT_NEAR(s.weights.x, 0.75f, 1e-5f);
            EXPECT_EQ(s.joints[1], 0);
            EXPECT_NEAR(s.weights.y, 0.25f, 1e-5f);
        } else {
            EXPECT_EQ(s.joints[0], 0);
            EXPECT_FLOAT_EQ(s.weights.x, 1.0f);
        }
    }

    ASSERT_EQ(model->animations.size(), 1u);
    const ImportedAnimation& slide = model->animations[0];
    EXPECT_EQ(slide.name, "Slide");
    EXPECT_NEAR(slide.duration, 1.0f, 1e-3f);
    std::vector<NodePose> poses(model->nodes.size());
    sampleAnimation(slide, 0.5f, poses);
    EXPECT_NEAR(poses[static_cast<std::size_t>(bone)].translation.x, 1.0f, 0.05f);
    EXPECT_NEAR(poses[static_cast<std::size_t>(bone)].translation.y, 1.0f, 1e-3f);
    sampleAnimation(slide, 1.0f, poses);
    EXPECT_NEAR(poses[static_cast<std::size_t>(bone)].translation.x, 2.0f, 1e-3f);
}

TEST(FbxImport, RejectsBrokenFiles) {
    test::TempDir dir;
    std::ofstream(dir.path() / "broken.fbx") << "not an fbx at all";
    EXPECT_FALSE(importModel(dir.path() / "broken.fbx").has_value());
    EXPECT_FALSE(importModel(dir.path() / "missing.obj").has_value());
}

TEST(FbxImport, BlendShapesBecomeMorphTargets) {
    test::TempDir dir;
    std::ofstream(dir.path() / "face.fbx") << R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Tri", "Mesh" {
		Vertices: *9 {
			a: 0,0,0,1,0,0,0,1,0
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		GeometryVersion: 124
	}
	Geometry: 1100, "Geometry::Smile", "Shape" {
		Version: 100
		Indexes: *1 {
			a: 2
		}
		Vertices: *3 {
			a: 0.5,0,0
		}
	}
	Model: 2000, "Model::Face", "Mesh" {
		Version: 232
	}
	Deformer: 5100, "Deformer::Morph", "BlendShape" {
		Version: 100
	}
	Deformer: 5101, "SubDeformer::Smile", "BlendShapeChannel" {
		Version: 100
		DeformPercent: 40
		FullWeights: *1 {
			a: 100
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
	C: "OO",5100,1000
	C: "OO",5101,5100
	C: "OO",1100,5101
}
)";
    auto model = importModel(dir.path() / "face.fbx");
    ASSERT_TRUE(model.has_value()) << model.error().message;
    ASSERT_EQ(model->meshes.size(), 1u);
    const ImportedMesh& mesh = model->meshes[0];
    ASSERT_EQ(mesh.data.morphs.size(), 1u);
    ASSERT_EQ(mesh.weights.size(), 1u);
    EXPECT_NEAR(mesh.weights[0], 0.4f, 1e-5f) << "DeformPercent 40";
    EXPECT_EQ(mesh.morphNames[0], "Smile");
    for (std::size_t v = 0; v < mesh.data.vertices.size(); ++v) {
        const bool top = mesh.data.vertices[v].position.y > 0.5f;
        EXPECT_FLOAT_EQ(mesh.data.morphs[0].positions[v].x, top ? 0.5f : 0.0f) << "only the shape's vertex moves";
    }
}

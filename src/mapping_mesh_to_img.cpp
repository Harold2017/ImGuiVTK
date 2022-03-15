#include <vtkSmartPointer.h>
#include <vtkActor.h>

// use Adobe spectrum ImGUI fork
#include "imgui_spectrum.h"  // use light theme in default
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiVTK.h"
#include "imfilebrowser.h"
#include "load3d.h"
#include "loadimg.h"
#include "mapping_mesh_to_img.h"
#include "metric.h"
#include <stdio.h>

#include <filesystem>

#include <glad/glad.h>

// Include glfw3.h after our OpenGL definitions
#include <GLFW/glfw3.h>

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

void handle_key_press(vtkCamera* camera, vtkActor* model_actor, double* elevation, double* azimuth, float model_move_resolution);

int main(int argc, char* argv[])
{
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1920, 1080, "Annotation Tool", NULL, NULL);
    if (window == NULL)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    bool err = gladLoadGL() == 0;
    if (err)
    {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Setup Dear ImGui style
    //ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();
    ImGui::Spectrum::StyleColorsSpectrum();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // setup vtk
    vtkNew<vtkGenericRenderWindowInteractor> interactor;
    ImGuiVTK instance;
    instance.Interactor = interactor;
    instance.Init();
    auto props = vtkSmartPointer<vtkPropCollection>::New();

    // file browser
    ImGui::FileBrowser imgFileDialog;
    imgFileDialog.SetTitle("ImgFileSelection");
    imgFileDialog.SetTypeFilters({ ".jpg", "jpeg", ".bmp", ".png", ".tif", ".pnm" });
    ImGui::FileBrowser meshFileDialog;
    meshFileDialog.SetTitle("MeshFileSelection");
    meshFileDialog.SetTypeFilters({ ".stl", ".obj" });

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    ImVec2 button_size(ImGui::GetFontSize() * 7.0f, 0.0f);

    std::string MeshFileName{}, ImgFileName{};
    vtkSmartPointer<vtkPolyData> PolyData = nullptr;
    vtkSmartPointer<vtkImageData> ImgData = nullptr;
    SceneAndBackground SceneAndImg{};
    bool MeshChanged = false;

    float inplane_rot_angle = 0, current_roll = 0;
    float elevation = 0; double current_elevation = 0;
    float azimuth = 0; double current_azimuth = 0;
    float zoom = 1, last_zoom = 1;
    float camera_move_resolution = 1, model_move_resolution = 1;
    float model_opacity = 0.5;
    ImVec4 model_color = ImVec4(0.00f, 0.00f, 1.00f, 1.00f);
    bool truncated = false, occluded = false;

    std::set<std::string> LockedMeshes;
    std::string OutputFilename{};
    std::string ModelCategory{};
    Metrics CurrentMetrics;

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // file browser
        if (ImGui::Begin("MeshFileBrowser"))
        {
            if (ImGui::Button("Open Mesh File"))
                meshFileDialog.Open();
        }
        ImGui::Text(MeshFileName.c_str());
        ImGui::End();
        meshFileDialog.Display();
        if (meshFileDialog.HasSelected())
        {
            // setup
            if (MeshFileName.empty())
            {
                MeshFileName = meshFileDialog.GetSelected().string();
                MeshChanged = true;
            }
            else  // change the mesh
            {
                auto tmp = meshFileDialog.GetSelected().string();
                if (MeshFileName != tmp)
                {
                    MeshFileName = tmp;
                    MeshChanged = true;
                }
                else
                    MeshChanged = false;
            }
            if (MeshChanged)
            {
                PolyData = ReadPolyData(MeshFileName.c_str());
                SetupModelRender(instance.Renderer, PolyData);
                // replace scene mesh only if the scene has been setup
                if (SceneAndImg.SceneActor != nullptr)
                    ChangeTheModel(SceneAndImg, PolyData);
            }
            // TODO: setup two renderers on ImGuiVTK instance.init() and make change mesh / image easier (one props for one)
            meshFileDialog.ClearSelected();
        }
        if (ImGui::Begin("ImgFileBrowser"))
        {
            if (ImGui::Button("Open Image File"))
                imgFileDialog.Open();
        }
        ImGui::Text(ImgFileName.c_str());
        ImGui::End();
        imgFileDialog.Display();
        if (imgFileDialog.HasSelected())
        {
            ImgFileName = imgFileDialog.GetSelected().string();
            ImgData = ReadImageData(ImgFileName.c_str());
            // setup
            if (SceneAndImg.BackgroundActor == nullptr)
                SceneAndImg = SetupSceneAndBackgroundRenders(instance.RenderWindow, ImgData, PolyData);
            else // replace the background image
            {
                ChangeTheBackgroundImage(SceneAndImg, ImgData);
            }
            // first let the scene camera follows the model camera
            SceneAndImg.SceneRenderer->SetActiveCamera(instance.Renderer->GetActiveCamera());
            imgFileDialog.ClearSelected();

            auto path = std::filesystem::path(ImgFileName);
            CurrentMetrics.image_name = path.stem().string();
            CurrentMetrics.image_path = ImgFileName;
            CurrentMetrics.metrics.clear();
            LockedMeshes.clear();
            OutputFilename = path.replace_extension(".txt").string();
        }

        if (ImGui::Begin("Overlay"))
        {
            // show following items only when both model and image are loaded
            if (SceneAndImg.SceneRenderer != nullptr)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0.2f, 0.3f, 0.4f, 1.0f });
                if (ImGui::Button("Lock Overlay"))
                {
                    vtkNew<vtkCamera> new_camera;
                    new_camera->DeepCopy(instance.Renderer->GetActiveCamera());  // make a deep copy to break the camera dependecy of two renderers
                    SceneAndImg.SceneRenderer->SetActiveCamera(new_camera);
                    current_roll = new_camera->GetRoll();
                }
                ImGui::SameLine();
                if (ImGui::Button("Release Overlay"))
                {
                    SceneAndImg.SceneRenderer->SetActiveCamera(instance.Renderer->GetActiveCamera());
                }
                ImGui::PopStyleColor(1);

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // scene model opacity
                if (ImGui::SliderFloat("Model Opacity", &model_opacity, 0.1f, 1.0f))
                {
                    SceneAndImg.SceneActor->GetProperty()->SetOpacity(model_opacity);
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // scene model color adjustment
                if (ImGui::ColorEdit3("Model Color", (float*)&model_color))
                {
                    double color[3] = { model_color.x, model_color.y, model_color.z };
                    SceneAndImg.SceneActor->GetProperty()->SetAmbientColor(color);
                    SceneAndImg.SceneActor->GetProperty()->SetDiffuseColor(color);
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // in-plane rotation
                if (ImGui::SliderFloat("In-Plane Rotation Angle", &inplane_rot_angle, -180.0f, 180.0f, "%.1f degrees"))
                {
                    auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                    current_cam->SetRoll(current_roll + inplane_rot_angle);
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // zoom
                if (ImGui::SliderFloat("Zoom", &zoom, 0.5f, 1.5f))
                {
                    auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                    current_cam->Zoom(zoom / last_zoom);
                    last_zoom = zoom;
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                float middle = ImGui::GetWindowContentRegionMax().x / 2;
                float upDownIndent = middle - 60; //middle - ImGui::GetFontSize() * strlen("Camera Up") / 2;
                float rightIndent = middle * 2 - 60 * 2 - 10;

                // // camera move resolution
                // ImGui::SliderFloat("Camera Move Resolution", &camera_move_resolution, 0.1f, 2.0f);

                // // camera up/down/left/right (keep view up)
                // ImGui::Indent(upDownIndent);
                // if (ImGui::Button("Camera Up", ImVec2(120, 30))) {
                //     auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                //     double pos[3];
                //     current_cam->GetPosition(pos);
                //     pos[1] += camera_move_resolution;
                //     current_cam->SetPosition(pos);
                // }
                // ImGui::Unindent(upDownIndent);
                // if (ImGui::Button("Camera Left", ImVec2(120, 30))) {
                //     auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                //     double pos[3];
                //     current_cam->GetPosition(pos);
                //     pos[0] -= camera_move_resolution;
                //     current_cam->SetPosition(pos);
                // }
                // ImGui::SameLine();
                // ImGui::Indent(upDownIndent);
                // if (ImGui::Button("Camera Down", ImVec2(120, 30))) {
                //     auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                //     double pos[3];
                //     current_cam->GetPosition(pos);
                //     pos[1] -= camera_move_resolution;
                //     current_cam->SetPosition(pos);
                // }
                // ImGui::Unindent(upDownIndent);
                // ImGui::SameLine();
                // ImGui::Indent(rightIndent);
                // if (ImGui::Button("Camera Right", ImVec2(120, 30))) {
                //     auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                //     double pos[3];
                //     current_cam->GetPosition(pos);
                //     pos[0] += camera_move_resolution;
                //     current_cam->SetPosition(pos);
                // }
                // ImGui::Unindent(rightIndent);

                // ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // elevation rotation (will change view up)
                if (ImGui::SliderFloat("Elevation", &elevation, -180.0f, 180.0f, "%.1f degrees"))
                {
                    auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                    current_cam->Elevation(elevation - current_elevation);
                    current_elevation = elevation;
                    current_cam->OrthogonalizeViewUp();
                    // double* p = current_cam->GetViewUp();
                    // printf("%f, %f, %f\n", p[0], p[1], p[2]);
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // azimuth rotation (not change view up)
                if (ImGui::SliderFloat("Azimuth", &azimuth, -180.0f, 180.0f, "%.1f degrees"))
                {
                    auto current_cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                    current_cam->Azimuth(azimuth - current_azimuth);
                    current_azimuth = azimuth;
                }

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // camera move resolution
                ImGui::SliderFloat("Model Move Resolution", &model_move_resolution, 0.1f, 2.0f);

                // model up/down/left/right
                ImGui::Indent(upDownIndent);
                if (ImGui::Button("Model Up", ImVec2(120, 30))) {
                    SceneAndImg.SceneActor->AddPosition(0, model_move_resolution, 0);
                }
                ImGui::Unindent(upDownIndent);
                if (ImGui::IsItemActive()) SceneAndImg.SceneActor->AddPosition(0, model_move_resolution, 0);  // hold button
                if (ImGui::Button("Model Left", ImVec2(120, 30))) {
                    SceneAndImg.SceneActor->AddPosition(-model_move_resolution, 0, 0);
                }
                if (ImGui::IsItemActive()) SceneAndImg.SceneActor->AddPosition(-model_move_resolution, 0, 0);  // hold button
                ImGui::SameLine();
                ImGui::Indent(upDownIndent);
                if (ImGui::Button("Model Down", ImVec2(120, 30))) {
                    SceneAndImg.SceneActor->AddPosition(0, -model_move_resolution, 0);
                }
                ImGui::Unindent(upDownIndent);
                if (ImGui::IsItemActive()) SceneAndImg.SceneActor->AddPosition(0, -model_move_resolution, 0);  // hold button
                ImGui::SameLine();
                ImGui::Indent(rightIndent);
                if (ImGui::Button("Model Right", ImVec2(120, 30))) {
                    SceneAndImg.SceneActor->AddPosition(model_move_resolution, 0, 0);
                }
                ImGui::Unindent(rightIndent);
                if (ImGui::IsItemActive()) SceneAndImg.SceneActor->AddPosition(model_move_resolution, 0, 0);  // hold button

                ImGui::Dummy(ImVec2(0.0f, 20.0f));
                
                // label truncated / occluded
                ImGui::Checkbox("Truncated", &truncated);
                ImGui::SameLine();
                ImGui::Checkbox("Occluded", &occluded);

                ImGui::Text("Model Category:");
                ImGui::InputTextWithHint("###category", "e.g. Mouse", &ModelCategory);

                ImGui::Dummy(ImVec2(0.0f, 20.0f));

                // project mesh to image and lock the image data
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 1.0f, 0.3f, 0.4f, 1.0f });
                if (ImGui::Button("Save Current Model Metrics"))
                {
                    // TODO: better way to combine the current background image data with the scene mesh model projection
                    // maybe project the bounding box or silhouette?
                    ChangeTheBackgroundImage(SceneAndImg, GetScreenShotImageData(SceneAndImg));

                    auto model_name = std::filesystem::path(MeshFileName).stem().string();

                    auto cam = SceneAndImg.SceneRenderer->GetActiveCamera();
                    auto camera_parameters = GetCameraParameters(cam);

                    CurrentMetrics.metrics.push_back(Metric{
                        model_name,
                        ModelCategory,
                        truncated,
                        occluded,
                        camera_parameters,
                        MeshFileName
                    });
                    LockedMeshes.insert(model_name);

                    // debug (to verify the correctness of rotaion angles)
                    // FIXME: is this debug way correct? is the camera parameters calculation way correct?
                    double cam_pos[3];
                    GetCameraPosition(camera_parameters.azimuth, camera_parameters.elevation, camera_parameters.distance, cam_pos);
                    cam->SetPosition(cam_pos);
                    cam->Elevation(-camera_parameters.elevation);
                    cam->OrthogonalizeViewUp();
                    cam->Azimuth(-camera_parameters.azimuth);
                }
                ImGui::PopStyleColor(1);

                if (ImGui::BeginListBox("Saved Models"))
                {
                    for (auto const& m : LockedMeshes)
                    {
                        ImGui::Selectable(m.c_str(), false);
                    }
                    ImGui::EndListBox();
                }

                ImGui::Dummy(ImVec2(0.0f, 30.0f));

                ImGui::Text("Ouput Filename:");
                ImGui::InputText("###filename", &OutputFilename);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 1.0f, 0.5f, 0.0f, 1.0f });
                if (ImGui::Button("Write Metrics To File"))
                {
                    WriteMetricsToFile(OutputFilename, CurrentMetrics);
                }
                ImGui::PopStyleColor(1);
            }
        }
        ImGui::End();

        // Rendering

        instance.Render();
        ImGui::Render();

        // Handle key press events
        if (SceneAndImg.SceneRenderer != nullptr && SceneAndImg.SceneActor != nullptr)
            handle_key_press(SceneAndImg.SceneRenderer->GetActiveCamera(), SceneAndImg.SceneActor, &current_elevation, &current_azimuth, model_move_resolution);

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glScissor(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    instance.ShutDown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

void handle_key_press(vtkCamera* camera, vtkActor* model_actor, double* elevation, double* azimuth, float model_move_resolution)
{
    //for (int i = 0; i < IM_ARRAYSIZE(io.KeysDown); i++) if (ImGui::IsKeyPressed(i)) { std::cout << i << '\n'; }
    // GLFW keys layout: https://www.glfw.org/docs/3.3/group__keys.html
    const int keys[]{ 87, 65, 83, 68, 265, 263, 264, 262 };
    for (auto key : keys)
    {
        if (ImGui::IsKeyPressed(key))
        {
            if (key == 265)  // Up
            {
                camera->Elevation(1);
                *elevation = *elevation + 1;
                camera->OrthogonalizeViewUp();
            }
            if (key == 264)  // Down
            {
                camera->Elevation(-1);
                *elevation = *elevation - 1;
                camera->OrthogonalizeViewUp();
            }
            if (key == 263)  // Left
            {
                camera->Azimuth(-1);
                *azimuth = *azimuth - 1;
            }
            if (key == 262)  // Right
            {
                camera->Azimuth(1);
                *azimuth = *azimuth + 1;
            }

            if (key == 87)  // w
            {
                model_actor->AddPosition(0, model_move_resolution, 0);
            }
            if (key == 83)  // s
            {
                model_actor->AddPosition(0, -model_move_resolution, 0);
            }
            if (key == 65)  // a
            {
                model_actor->AddPosition(-model_move_resolution, 0, 0);
            }
            if (key == 68)  // d
            {
                model_actor->AddPosition(model_move_resolution, 0, 0);
            }
        }
    }
}
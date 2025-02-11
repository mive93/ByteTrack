#include "BYTETracker.h"
#include "NvInfer.h"
#include "NvInferPlugin.h"
#include "cuda_runtime_api.h"
#include "logging.h"
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <numeric>
#include <opencv2/opencv.hpp>
#include <sstream>
#include <vector>

#include "tkDNN/Yolo3Detection.h"

#define CHECK(status)                                                                              \
    do {                                                                                           \
        auto ret = (status);                                                                       \
        if (ret != 0) {                                                                            \
            std::cerr << "Cuda failure: " << ret << std::endl;                                     \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

#define DEVICE 0 // GPU id
#define NMS_THRESH 0.7
#define BBOX_CONF_THRESH 0.1

using namespace nvinfer1;

// stuff we know about the network and the input/output blobs
static const int INPUT_W = 1088;
static const int INPUT_H = 608;
const char* INPUT_BLOB_NAME = "input_0";
const char* OUTPUT_BLOB_NAME = "output_0";
static Logger gLogger;

cv::Mat static_resize(cv::Mat& img)
{
    float r = std::min(INPUT_W / (img.cols * 1.0), INPUT_H / (img.rows * 1.0));
    // r = std::min(r, 1.0f);
    int unpad_w = r * img.cols;
    int unpad_h = r * img.rows;
    cv::Mat re(unpad_h, unpad_w, CV_8UC3);
    cv::resize(img, re, re.size());
    cv::Mat out(INPUT_H, INPUT_W, CV_8UC3, cv::Scalar(114, 114, 114));
    re.copyTo(out(cv::Rect(0, 0, re.cols, re.rows)));
    return out;
}

struct GridAndStride
{
    int grid0;
    int grid1;
    int stride;
};

static void generate_grids_and_stride(const int target_w,
                                      const int target_h,
                                      std::vector<int>& strides,
                                      std::vector<GridAndStride>& grid_strides)
{
    for (auto stride : strides) {
        int num_grid_w = target_w / stride;
        int num_grid_h = target_h / stride;
        for (int g1 = 0; g1 < num_grid_h; g1++) {
            for (int g0 = 0; g0 < num_grid_w; g0++) {
                grid_strides.push_back((GridAndStride){ g0, g1, stride });
            }
        }
    }
}

static inline float intersection_area(const bytetrack::Object& a, const bytetrack::Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<bytetrack::Object>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j) {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j) {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

    // #pragma omp parallel sections
    {
        // #pragma omp section
        {
            if (left < j)
                qsort_descent_inplace(faceobjects, left, j);
        }
        // #pragma omp section
        {
            if (i < right)
                qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<bytetrack::Object>& objects)
{
    if (objects.empty())
        return;

    qsort_descent_inplace(objects, 0, objects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<bytetrack::Object>& faceobjects,
                              std::vector<int>& picked,
                              float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++) {
        areas[i] = faceobjects[i].rect.area();
    }

    for (int i = 0; i < n; i++) {
        const bytetrack::Object& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++) {
            const bytetrack::Object& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static void generate_yolox_proposals(std::vector<GridAndStride> grid_strides,
                                     float* feat_blob,
                                     float prob_threshold,
                                     std::vector<bytetrack::Object>& objects)
{
    const int num_class = 1;

    const int num_anchors = grid_strides.size();

    for (int anchor_idx = 0; anchor_idx < num_anchors; anchor_idx++) {
        const int grid0 = grid_strides[anchor_idx].grid0;
        const int grid1 = grid_strides[anchor_idx].grid1;
        const int stride = grid_strides[anchor_idx].stride;

        const int basic_pos = anchor_idx * (num_class + 5);

        // yolox/models/yolo_head.py decode logic
        float x_center = (feat_blob[basic_pos + 0] + grid0) * stride;
        float y_center = (feat_blob[basic_pos + 1] + grid1) * stride;
        float w = exp(feat_blob[basic_pos + 2]) * stride;
        float h = exp(feat_blob[basic_pos + 3]) * stride;
        float x0 = x_center - w * 0.5f;
        float y0 = y_center - h * 0.5f;

        float box_objectness = feat_blob[basic_pos + 4];
        for (int class_idx = 0; class_idx < num_class; class_idx++) {
            float box_cls_score = feat_blob[basic_pos + 5 + class_idx];
            float box_prob = box_objectness * box_cls_score;
            if (box_prob > prob_threshold) {
                bytetrack::Object obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = w;
                obj.rect.height = h;
                obj.label = class_idx;
                obj.prob = box_prob;

                objects.push_back(obj);
            }

        } // class loop

    } // point anchor loop
}

float* blobFromImage(cv::Mat& img)
{
    cv::cvtColor(img, img, cv::COLOR_BGR2RGB);

    float* blob = new float[img.total() * 3];
    int channels = 3;
    int img_h = img.rows;
    int img_w = img.cols;
    std::vector<float> mean = { 0.485, 0.456, 0.406 };
    std::vector<float> std = { 0.229, 0.224, 0.225 };
    for (int c = 0; c < channels; c++) {
        for (int h = 0; h < img_h; h++) {
            for (int w = 0; w < img_w; w++) {
                blob[c * img_w * img_h + h * img_w + w] =
                  (((float)img.at<cv::Vec3b>(h, w)[c]) / 255.0f - mean[c]) / std[c];
            }
        }
    }
    return blob;
}

static void decode_outputs(float* prob,
                           std::vector<bytetrack::Object>& objects,
                           float scale,
                           const int img_w,
                           const int img_h)
{
    std::vector<bytetrack::Object> proposals;
    std::vector<int> strides = { 8, 16, 32 };
    std::vector<GridAndStride> grid_strides;
    generate_grids_and_stride(INPUT_W, INPUT_H, strides, grid_strides);
    generate_yolox_proposals(grid_strides, prob, BBOX_CONF_THRESH, proposals);
    // std::cout << "num of boxes before nms: " << proposals.size() << std::endl;

    qsort_descent_inplace(proposals);

    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, NMS_THRESH);

    int count = picked.size();

    // std::cout << "num of boxes: " << count << std::endl;

    objects.resize(count);
    for (int i = 0; i < count; i++) {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x) / scale;
        float y0 = (objects[i].rect.y) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height) / scale;

        // clip
        // x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        // y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        // x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        // y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }
}

const float color_list[80][3] = {
    { 0.000, 0.447, 0.741 }, { 0.850, 0.325, 0.098 }, { 0.929, 0.694, 0.125 },
    { 0.494, 0.184, 0.556 }, { 0.466, 0.674, 0.188 }, { 0.301, 0.745, 0.933 },
    { 0.635, 0.078, 0.184 }, { 0.300, 0.300, 0.300 }, { 0.600, 0.600, 0.600 },
    { 1.000, 0.000, 0.000 }, { 1.000, 0.500, 0.000 }, { 0.749, 0.749, 0.000 },
    { 0.000, 1.000, 0.000 }, { 0.000, 0.000, 1.000 }, { 0.667, 0.000, 1.000 },
    { 0.333, 0.333, 0.000 }, { 0.333, 0.667, 0.000 }, { 0.333, 1.000, 0.000 },
    { 0.667, 0.333, 0.000 }, { 0.667, 0.667, 0.000 }, { 0.667, 1.000, 0.000 },
    { 1.000, 0.333, 0.000 }, { 1.000, 0.667, 0.000 }, { 1.000, 1.000, 0.000 },
    { 0.000, 0.333, 0.500 }, { 0.000, 0.667, 0.500 }, { 0.000, 1.000, 0.500 },
    { 0.333, 0.000, 0.500 }, { 0.333, 0.333, 0.500 }, { 0.333, 0.667, 0.500 },
    { 0.333, 1.000, 0.500 }, { 0.667, 0.000, 0.500 }, { 0.667, 0.333, 0.500 },
    { 0.667, 0.667, 0.500 }, { 0.667, 1.000, 0.500 }, { 1.000, 0.000, 0.500 },
    { 1.000, 0.333, 0.500 }, { 1.000, 0.667, 0.500 }, { 1.000, 1.000, 0.500 },
    { 0.000, 0.333, 1.000 }, { 0.000, 0.667, 1.000 }, { 0.000, 1.000, 1.000 },
    { 0.333, 0.000, 1.000 }, { 0.333, 0.333, 1.000 }, { 0.333, 0.667, 1.000 },
    { 0.333, 1.000, 1.000 }, { 0.667, 0.000, 1.000 }, { 0.667, 0.333, 1.000 },
    { 0.667, 0.667, 1.000 }, { 0.667, 1.000, 1.000 }, { 1.000, 0.000, 1.000 },
    { 1.000, 0.333, 1.000 }, { 1.000, 0.667, 1.000 }, { 0.333, 0.000, 0.000 },
    { 0.500, 0.000, 0.000 }, { 0.667, 0.000, 0.000 }, { 0.833, 0.000, 0.000 },
    { 1.000, 0.000, 0.000 }, { 0.000, 0.167, 0.000 }, { 0.000, 0.333, 0.000 },
    { 0.000, 0.500, 0.000 }, { 0.000, 0.667, 0.000 }, { 0.000, 0.833, 0.000 },
    { 0.000, 1.000, 0.000 }, { 0.000, 0.000, 0.167 }, { 0.000, 0.000, 0.333 },
    { 0.000, 0.000, 0.500 }, { 0.000, 0.000, 0.667 }, { 0.000, 0.000, 0.833 },
    { 0.000, 0.000, 1.000 }, { 0.000, 0.000, 0.000 }, { 0.143, 0.143, 0.143 },
    { 0.286, 0.286, 0.286 }, { 0.429, 0.429, 0.429 }, { 0.571, 0.571, 0.571 },
    { 0.714, 0.714, 0.714 }, { 0.857, 0.857, 0.857 }, { 0.000, 0.447, 0.741 },
    { 0.314, 0.717, 0.741 }, { 0.50, 0.5, 0 }
};

void doInference(IExecutionContext& context,
                 float* input,
                 float* output,
                 const int output_size,
                 cv::Size input_shape)
{
    const ICudaEngine& engine = context.getEngine();

    // Pointers to input and output device buffers to pass to engine.
    // Engine requires exactly IEngine::getNbBindings() number of buffers.
    assert(engine.getNbBindings() == 2);
    void* buffers[2];

    // In order to bind the buffers, we need to know the names of the input and output tensors.
    // Note that indices are guaranteed to be less than IEngine::getNbBindings()
    const int inputIndex = engine.getBindingIndex(INPUT_BLOB_NAME);

    assert(engine.getBindingDataType(inputIndex) == nvinfer1::DataType::kFLOAT);
    const int outputIndex = engine.getBindingIndex(OUTPUT_BLOB_NAME);
    assert(engine.getBindingDataType(outputIndex) == nvinfer1::DataType::kFLOAT);
    // int mBatchSize = engine.getMaxBatchSize();

    // Create GPU buffers on device
    CHECK(
      cudaMalloc(&buffers[inputIndex], 3 * input_shape.height * input_shape.width * sizeof(float)));
    CHECK(cudaMalloc(&buffers[outputIndex], output_size * sizeof(float)));

    // Create stream
    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));

    // DMA input batch data to device, infer on the batch asynchronously, and DMA output back to
    // host
    CHECK(cudaMemcpyAsync(buffers[inputIndex],
                          input,
                          3 * input_shape.height * input_shape.width * sizeof(float),
                          cudaMemcpyHostToDevice,
                          stream));
    context.enqueue(1, buffers, stream, nullptr);
    CHECK(cudaMemcpyAsync(
      output, buffers[outputIndex], output_size * sizeof(float), cudaMemcpyDeviceToHost, stream));
    cudaStreamSynchronize(stream);

    // Release stream and buffers
    cudaStreamDestroy(stream);
    CHECK(cudaFree(buffers[inputIndex]));
    CHECK(cudaFree(buffers[outputIndex]));
}

int tkDNNMain(int argc, char** argv)
{
    std::string net = "yolo4_fp32.rt";
    std::string input = "../../../../videos/palace.mp4";
    int n_classes = 80;
    int n_batch = 1;
    float conf_thresh = 0.3f;

    if (argc > 1)
        net = argv[1];
    if (argc > 2)
        input = argv[2];

    tk::dnn::Yolo3Detection yolo;
    yolo.init(net, n_classes, n_batch, conf_thresh);

    cv::VideoCapture cap(input);
    if (!cap.isOpened())
        return 0;

    // int img_w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    // int img_h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    int fps = cap.get(cv::CAP_PROP_FPS);
    long nFrame = static_cast<long>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    std::cout << "Total frames: " << nFrame << std::endl;

    cv::Mat frame;
    std::vector<cv::Mat> batch_frame;
    std::vector<cv::Mat> batch_dnn_input;

    cv::Mat img;
    bytetrack::BYTETracker tracker(fps, 30);
    int num_frames = 0;
    int total_ms = 0;
    while (true) {
        // clear data structures
        batch_dnn_input.clear();
        batch_frame.clear();

        // read new frame
        cap >> frame;
        if (!frame.data)
            break;

        // update data structures
        batch_frame.push_back(frame);
        batch_dnn_input.push_back(frame.clone());

        // do inference

        yolo.update(batch_dnn_input, n_batch);

        // get bb
        std::vector<bytetrack::Object> objects;

        for (size_t i = 0; i < yolo.detected.size(); ++i) {

            if (yolo.detected[i].cl == 0) {

                bytetrack::Object obj;
                obj.rect.x = yolo.detected[i].x;
                obj.rect.y = yolo.detected[i].y;
                obj.rect.width = yolo.detected[i].w;
                obj.rect.height = yolo.detected[i].h;
                obj.label = yolo.detected[i].cl;
                obj.prob = yolo.detected[i].prob;

                objects.push_back(obj);
            }
        }

        auto start = std::chrono::system_clock::now();
        // update tracker
        std::vector<bytetrack::STrack> output_stracks = tracker.update(objects);

        // get time
        auto end = std::chrono::system_clock::now();
        total_ms =
          total_ms + std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        num_frames++;
        if (num_frames % 20 == 0)
            std::cout << "Processing frame " << num_frames << " ("
                      << total_ms / 1000.0f / num_frames << " ms avg)" << std::endl;

        // draw
        for (size_t i = 0; i < output_stracks.size(); i++) {
            std::vector<float> tlwh = output_stracks[i].tlwh;
            bool vertical = tlwh[2] / tlwh[3] > 1.6;
            if (tlwh[2] * tlwh[3] > 20 && !vertical) {
                cv::Scalar s = tracker.get_color(output_stracks[i].track_id);
                cv::putText(frame,
                            cv::format("%d", output_stracks[i].track_id),
                            cv::Point(tlwh[0], tlwh[1] - 5),
                            0,
                            0.6,
                            cv::Scalar(0, 0, 255),
                            2,
                            cv::LINE_AA);
                cv::rectangle(frame, cv::Rect(tlwh[0], tlwh[1], tlwh[2], tlwh[3]), s, 2);
            }
        }
        cv::putText(frame,
                    cv::format("frame: %d fps: %d num: %ld",
                               num_frames,
                               num_frames * 1000000 / total_ms,
                               output_stracks.size()),
                    cv::Point(0, 30),
                    0,
                    0.6,
                    cv::Scalar(0, 0, 255),
                    2,
                    cv::LINE_AA);

        cv::imshow("detection", frame);
        cv::waitKey(1);
    }

    cap.release();
    std::cout << "FPS: " << num_frames * 1000000 / total_ms << std::endl;

    return 0;
}

int main(int argc, char** argv)
{

    return tkDNNMain(argc, argv);

    cudaSetDevice(DEVICE);

    // create a model using the API directly and serialize it to a stream
    char* trtModelStream{ nullptr };
    size_t size{ 0 };

    if (argc == 4 && std::string(argv[2]) == "-i") {
        const std::string engine_file_path{ argv[1] };
        std::ifstream file(engine_file_path, std::ios::binary);
        if (file.good()) {
            file.seekg(0, file.end);
            size = file.tellg();
            file.seekg(0, file.beg);
            trtModelStream = new char[size];
            assert(trtModelStream);
            file.read(trtModelStream, size);
            file.close();
        }
    } else {
        std::cerr << "arguments not right!" << std::endl;
        std::cerr << "run 'python3 tools/trt.py -f exps/example/mot/yolox_s_mix_det.py -c "
                     "pretrained/bytetrack_s_mot17.pth.tar' to serialize model first!"
                  << std::endl;
        std::cerr << "Then use the following command:" << std::endl;
        std::cerr << "cd demo/TensorRT/cpp/build" << std::endl;
        std::cerr << "./bytetrack ../../../../YOLOX_outputs/yolox_s_mix_det/model_trt.engine -i "
                     "../../../../videos/palace.mp4  // deserialize file and run inference"
                  << std::endl;
        return -1;
    }
    const std::string input_video_path{ argv[3] };

    IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
    // bool didInitPlugins = ::initLibNvInferPlugins(nullptr, "");
    ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream, size);
    assert(engine != nullptr);
    IExecutionContext* context = engine->createExecutionContext();
    assert(context != nullptr);
    delete[] trtModelStream;
    auto out_dims = engine->getBindingDimensions(1);
    auto output_size = 1;
    for (int j = 0; j < out_dims.nbDims; j++) {
        output_size *= out_dims.d[j];
    }
    static float* prob = new float[output_size];

    cv::VideoCapture cap(input_video_path);
    if (!cap.isOpened())
        return 0;

    int img_w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int img_h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    int fps = cap.get(cv::CAP_PROP_FPS);
    long nFrame = static_cast<long>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    std::cout << "Total frames: " << nFrame << std::endl;

    cv::VideoWriter writer(
      "demo.mp4", cv::VideoWriter::fourcc('m', 'p', '4', 'v'), fps, cv::Size(img_w, img_h));

    cv::Mat img;
    bytetrack::BYTETracker tracker(fps, 30);
    int num_frames = 0;
    int total_ms = 0;
    while (true) {
        if (!cap.read(img))
            break;
        num_frames++;
        if (num_frames % 20 == 0) {
            std::cout << "Processing frame " << num_frames << " ("
                      << num_frames * 1000000 / total_ms << " fps)" << std::endl;
        }
        if (img.empty())
            break;
        cv::Mat pr_img = static_resize(img);

        float* blob;
        blob = blobFromImage(pr_img);
        float scale = std::min(INPUT_W / (img.cols * 1.0), INPUT_H / (img.rows * 1.0));

        // run inference
        auto start = std::chrono::system_clock::now();
        doInference(*context, blob, prob, output_size, pr_img.size());
        std::vector<bytetrack::Object> objects;
        decode_outputs(prob, objects, scale, img_w, img_h);
        std::vector<bytetrack::STrack> output_stracks = tracker.update(objects);
        auto end = std::chrono::system_clock::now();
        total_ms =
          total_ms + std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

        for (size_t i = 0; i < output_stracks.size(); i++) {
            std::vector<float> tlwh = output_stracks[i].tlwh;
            bool vertical = tlwh[2] / tlwh[3] > 1.6;
            if (tlwh[2] * tlwh[3] > 20 && !vertical) {
                cv::Scalar s = tracker.get_color(output_stracks[i].track_id);
                cv::putText(img,
                            cv::format("%d", output_stracks[i].track_id),
                            cv::Point(tlwh[0], tlwh[1] - 5),
                            0,
                            0.6,
                            cv::Scalar(0, 0, 255),
                            2,
                            cv::LINE_AA);
                cv::rectangle(img, cv::Rect(tlwh[0], tlwh[1], tlwh[2], tlwh[3]), s, 2);
            }
        }
        cv::putText(img,
                    cv::format("frame: %d fps: %d num: %ld",
                               num_frames,
                               num_frames * 1000000 / total_ms,
                               output_stracks.size()),
                    cv::Point(0, 30),
                    0,
                    0.6,
                    cv::Scalar(0, 0, 255),
                    2,
                    cv::LINE_AA);
        cv::imshow("img", img);
        writer.write(img);

        delete blob;
        char c = cv::waitKey(1);
        if (c > 0) {
            break;
        }
    }
    cap.release();
    std::cout << "FPS: " << num_frames * 1000000 / total_ms << std::endl;
    // destroy the engine
    context->destroy();
    engine->destroy();
    runtime->destroy();
    return 0;
}

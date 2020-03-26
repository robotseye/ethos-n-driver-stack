//
// Copyright © 2018-2020 Arm Limited. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//

#include "NetworkToGraphConverter.hpp"

#include "GraphNodes.hpp"
#include "Utils.hpp"

using namespace ethosn::support_library::utils;

namespace ethosn
{
namespace support_library
{

namespace
{

std::vector<Node*> CreateTransposeConv(Graph& graph,
                                       const Stride& stride,
                                       const TensorInfo& weightsInfo,
                                       const std::vector<uint8_t>& weightsData,
                                       const TensorInfo& biasInfo,
                                       std::vector<int32_t> biasData,
                                       const Padding& padding,
                                       const TensorInfo& inputInfo,
                                       const TensorInfo& outputInfo,
                                       const uint32_t sourceOperationId)
{
    std::vector<Node*> nodes;

    // TransposeConvolution is implemented as an upscale (padding) operation + a convolution.
    // The stride parameter of a TransposeConvolution represents the upscaling factor.
    // The stride of the convolution operation underneath is always 1.
    // The stride comes in as a vector {x, y} where x = y (validated by IsSupported checks)
    assert(stride.m_X == stride.m_Y);
    uint32_t upscaleFactor          = stride.m_X;
    const TensorShape& weightsShape = weightsInfo.m_Dimensions;

    // The padding of a TransposeConvolution affects the convolution operation underneath, but requires modification.
    // This means there is a restriction on the size of the padding such that our internal padding cannot be negative,
    // which is checked in IsTransposeConvolutionSupported (by virtue of supporting only same/valid padding).

    // The user-specified padding applies to the *output* of the transpose conv rather than the input like in a regular
    // convolution (see below example of output tensor with 1 padding on top/left). The padding is essentially cropping
    // the output tensor.
    //
    // When the padding is specified as zero the output tensor is not cropped at all, meaning that the top-left-most
    // (s_x, s_y) elements (where s_x, s_y are the strides) are equal to top-left (s_x, s_y) portion of the kernel
    // multiplied by the top-left input value.
    //
    // In order to get this same result from our internal convolution we need to add enough padding so that as we slide
    // the kernel over the upscaled-and-padded input, the first (s_x, s_y) output elements depend only on the top-left
    // input value. Here is an example showing that we need 2 padding for a 3x3 kernel with stride 2. The highlighted
    // window shows the values used to calculate the (1,1) output value and it depends only on I0 as required.
    // The same is true for the (0,0), (0,1) and (1,0) output values.
    //
    // +---+---+----+---+----+---+
    // | P | P | P  | P | P  | P |
    // +---╬═══╬════╬═══╬----+---+
    // | P ║ P | P  | P ║ P  | P |
    // +---╬---+----+---╬----+---+
    // | P ║ P | I0 | 0 ║ I1 | 0 |
    // +---╬---+----+---╬----+---+
    // | P ║ P | 0  | 0 ║ 0  | 0 |
    // +---╬═══╬════╬═══╬----+---+
    // | P | P | I2 | 0 | I3 | 0 |
    // +---+---+----+---+----+---+
    // | P | P | 0  | 0 | 0  | 0 |
    // +---+---+----+---+----+---+
    //
    // The amount of padding required for the zero-padding case is therefore kernel_size - 1.
    // Increasing the padding on the transpose convolution crops pixels from the output, which means that the region of
    // the output which depends only on the first input value gets smaller. This means that for our internal convolution
    // we must *decrease* the padding by the same amount. At the extreme this means that we will have zero padding
    // on our internal convolution so that *only* the first output value will depend on the first input value.
    // This corresponds to a padding/cropping of kernel_size - 1 on the transpose convolution.
    //
    // From this, we can calculate the internal convolution padding as: kernel_size - 1 - original_padding.
    const uint32_t topMcePadding  = weightsShape[0] - 1 - padding.m_Top;
    const uint32_t leftMcePadding = weightsShape[1] - 1 - padding.m_Left;

    TensorShape inputShape = inputInfo.m_Dimensions;

    // We can't do upscaling with a large kernel size, so we have to do the upscaling in a separate pass beforehand
    // with an identity (1x1) kernel. The convolution is then performed in another pass.
    if (weightsShape[0] > 7 || weightsShape[1] > 7)
    {
        const TensorShape& intermediateOutputShape = { inputShape[0], inputShape[1] * upscaleFactor,
                                                       inputShape[2] * upscaleFactor, inputShape[3] };

        const uint32_t numIfm   = inputShape[3];
        const float weightScale = 0.5f;
        const float biasScale   = weightScale * inputInfo.m_QuantizationInfo.m_Scale;

        std::vector<uint8_t> weightsData(1 * 1 * 1 * numIfm, 2);
        std::vector<int32_t> biasData(numIfm, 0);

        TensorInfo weightInfo{ { 1, 1, numIfm, 1 }, DataType::UINT8_QUANTIZED, DataFormat::HWIM, { 0, weightScale } };
        TensorInfo biasInfo{ { 1, 1, 1, numIfm }, DataType::INT32_QUANTIZED, DataFormat::NHWC, { 0, biasScale } };

        MceOperationNode* identityDepthwiseNode = graph.CreateAndAddNode<MceOperationNode>(
            inputShape, intermediateOutputShape, inputInfo.m_QuantizationInfo, weightInfo, weightsData, biasInfo,
            biasData, Stride{ 1, 1 }, upscaleFactor, 0, 0, ethosn::command_stream::MceOperation::DEPTHWISE_CONVOLUTION,
            CompilerDataFormat::NHWCB, std::set<uint32_t>{ sourceOperationId });
        nodes.push_back(identityDepthwiseNode);

        upscaleFactor = 1;
        inputShape    = intermediateOutputShape;
    }

    // Rotate weights by 180 in the XY plane.
    // This is needed for the internal convolution to produce the same result as the transpose convolution.
    utils::ConstTensorData originalWeights(weightsData.data(), weightsShape);
    std::vector<uint8_t> flippedWeightsData(weightsData.size());
    utils::TensorData flippedWeights(flippedWeightsData.data(), weightsShape);
    for (uint32_t y = 0; y < weightsShape[0]; ++y)
    {
        for (uint32_t x = 0; x < weightsShape[1]; ++x)
        {
            // The other two dimensions are irrelevant and we can copy them together as a contiguous block
            const uint32_t n   = weightsShape[2] * weightsShape[3];
            const uint8_t& src = originalWeights.GetElementRef(y, x, 0, 0);
            uint8_t& dst       = flippedWeights.GetElementRef(weightsShape[0] - 1 - y, weightsShape[1] - 1 - x, 0, 0);
            std::copy_n(&src, n, &dst);
        }
    }

    MceOperationNode* convNode = graph.CreateAndAddNode<MceOperationNode>(
        inputShape, outputInfo.m_Dimensions, outputInfo.m_QuantizationInfo, weightsInfo, flippedWeightsData, biasInfo,
        std::move(biasData), Stride(1, 1), upscaleFactor, topMcePadding, leftMcePadding,
        command_stream::MceOperation::CONVOLUTION, CompilerDataFormat::NHWCB, std::set<uint32_t>{ sourceOperationId });

    nodes.push_back(convNode);

    return nodes;
}

}    // namespace

void NetworkToGraphConverter::Visit(Reshape& reshape)
{
    std::vector<Node*> nodes;
    // Add conversion to NHWC (if necessary), then reinterpret to new shape, then conversion back to NHWCB.
    if (m_OperandToNode[&reshape.GetInput(0)]->GetFormat() != CompilerDataFormat::NHWC)
    {
        FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
            reshape.GetInput(0).GetTensorInfo().m_Dimensions, reshape.GetInput(0).GetTensorInfo().m_QuantizationInfo,
            CompilerDataFormat::NHWC, std::set<uint32_t>{ reshape.GetId() });
        nodes.push_back(conversionNode);
    }

    ReinterpretNode* reinterpretNode = m_Graph.CreateAndAddNode<ReinterpretNode>(
        reshape.GetOutput(0).GetTensorInfo().m_Dimensions, reshape.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
        CompilerDataFormat::NHWC, std::set<uint32_t>{ reshape.GetId() });
    nodes.push_back(reinterpretNode);

    FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
        reshape.GetOutput(0).GetTensorInfo().m_Dimensions, reshape.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
        CompilerDataFormat::NHWCB, std::set<uint32_t>{ reshape.GetId() });
    nodes.push_back(conversionNode);

    ConnectNodeChain(reshape, nodes);
}

void NetworkToGraphConverter::Visit(Pooling& pooling)
{
    const auto createFuseOnlyPleNode = [&](const command_stream::PleOperation op) {
        utils::ShapeMultiplier shapeMultiplier = { { 1, pooling.GetPoolingInfo().m_PoolingStrideY },
                                                   { 1, pooling.GetPoolingInfo().m_PoolingStrideX },
                                                   1 };
        return m_Graph.CreateAndAddNode<FuseOnlyPleOperationNode>(
            pooling.GetOutput(0).GetTensorInfo().m_Dimensions, pooling.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
            op, CompilerDataFormat::NHWCB, shapeMultiplier, std::set<uint32_t>{ pooling.GetId() });
    };

    const auto createStandalonePleNode = [&](const command_stream::PleOperation op) {
        return m_Graph.CreateAndAddNode<StandalonePleOperationNode>(
            pooling.GetOutput(0).GetTensorInfo().m_Dimensions, pooling.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
            op, CompilerDataFormat::NHWCB, std::set<uint32_t>{ pooling.GetId() });
    };

    Node* n = nullptr;

    const uint32_t inputHeight = pooling.GetInput(0).GetTensorInfo().m_Dimensions[1];
    const uint32_t inputWidth  = pooling.GetInput(0).GetTensorInfo().m_Dimensions[2];

    const PoolingInfo& poolingInfo = pooling.GetPoolingInfo();

    const PoolingInfo poolingInfoIfMean = {
        inputWidth,
        inputHeight,
        poolingInfo.m_PoolingStrideX,
        poolingInfo.m_PoolingStrideY,
        Padding{ 0, 0, 0, 0 },
        PoolingType::AVG,
    };

    const SupportedLevel supportedLevel = IsPoolingSupported(poolingInfo, pooling.GetInput(0).GetTensorInfo());

    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        const auto& outInfo = pooling.GetOutput(0).GetTensorInfo();
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ pooling.GetId() });
        ConnectNode(pooling, n);
        return;
    }

    if (poolingInfo == poolingInfoIfMean)
    {
        n = createFuseOnlyPleNode(command_stream::PleOperation::MEAN_XY_8X8);
    }
    else if (poolingInfo == PoolingInfo{ 3, 3, 1, 1, poolingInfo.m_Padding, PoolingType::AVG })
    {
        n = createStandalonePleNode(command_stream::PleOperation::AVGPOOL_3X3_1_1_UDMA);
    }
    else if (poolingInfo == PoolingInfo{ 2, 2, 2, 2, poolingInfo.m_Padding, PoolingType::MAX })
    {
        n = createFuseOnlyPleNode(command_stream::PleOperation::MAXPOOL_2X2_2_2);
    }
    else if (poolingInfo == PoolingInfo{ 3, 3, 2, 2, poolingInfo.m_Padding, PoolingType::MAX })
    {
        n = createFuseOnlyPleNode(command_stream::PleOperation::MAXPOOL_3X3_2_2);
    }
    else
    {
        assert(!"Unsupported");
    }

    ConnectNode(pooling, n);
}

void NetworkToGraphConverter::Visit(Sigmoid& sigmoid)
{
    Node* const pleSigmoid = m_Graph.CreateAndAddNode<FuseOnlyPleOperationNode>(
        sigmoid.GetOutput(0).GetTensorInfo().m_Dimensions, sigmoid.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
        command_stream::PleOperation::SIGMOID, CompilerDataFormat::NHWCB, g_IdentityShapeMultiplier,
        std::set<uint32_t>{ sigmoid.GetId() });

    ConnectNode(sigmoid, pleSigmoid);
}

void NetworkToGraphConverter::Visit(Softmax& softmax)
{
    const SupportedLevel supportedLevel = IsSoftmaxSupported(softmax.GetInput(0).GetTensorInfo());

    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        const auto& outInfo = softmax.GetOutput(0).GetTensorInfo();
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ softmax.GetId() });
        ConnectNode(softmax, n);
        return;
    }
    assert(!"Not implemented");
}

void NetworkToGraphConverter::Visit(Relu& relu)
{
    Node* n = m_Graph.CreateAndAddNode<McePostProcessOperationNode>(
        relu.GetOutput(0).GetTensorInfo().m_Dimensions, relu.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
        relu.GetReluInfo().m_LowerBound, relu.GetReluInfo().m_UpperBound, CompilerDataFormat::NHWCB,
        std::set<uint32_t>{ relu.GetId() });
    ConnectNode(relu, n);
}

namespace
{

std::vector<uint8_t> Pad(const std::vector<uint8_t>& input, size_t newSize, uint8_t padValue)
{
    std::vector<uint8_t> result = input;
    result.resize(newSize, padValue);
    return result;
}

}    // namespace

void NetworkToGraphConverter::Visit(FullyConnected& fullyConnected)
{
    std::vector<Node*> nodes;

    // Input to FC must be NHWC
    // Add conversion node if necessary
    if (m_OperandToNode[&fullyConnected.GetInput(0)]->GetFormat() != CompilerDataFormat::NHWC)
    {
        FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
            fullyConnected.GetInput(0).GetTensorInfo().m_Dimensions,
            fullyConnected.GetInput(0).GetTensorInfo().m_QuantizationInfo, CompilerDataFormat::NHWC,
            std::set<uint32_t>{ fullyConnected.GetId() });
        nodes.push_back(conversionNode);
    }

    // However we interpret it as NHWCB so that it gets copied without conversion into SRAM.
    // We choose the smallest shape that will encompass all the data when it is interpreted in brick format.
    auto GetShapeContainingLinearElements = [](const TensorShape& brickGroupShape, uint32_t numElements) {
        const uint32_t brickGroupHeight           = brickGroupShape[1];
        const uint32_t brickGroupWidth            = brickGroupShape[2];
        const uint32_t brickGroupChannels         = brickGroupShape[3];
        const uint32_t patchHeight                = 4;
        const uint32_t patchWidth                 = 4;
        const uint32_t patchesPerBrickGroupHeight = brickGroupHeight / patchHeight;
        const uint32_t patchesPerBrickGroupWidth  = brickGroupWidth / patchWidth;
        const uint32_t patchesPerBrickGroup =
            patchesPerBrickGroupHeight * patchesPerBrickGroupWidth * brickGroupChannels;

        // If there are less than one bricks worth of elements then we can have a tensor with a single patch in XY
        // and up to 16 channels.
        // If there are between one and two bricks worth of elements then we can have a tensor with a column of two
        // patches in XY and 16 channels. Note we always need 16 channels in this case as the first brick is full.
        // If there are between two and four bricks worth of elements then we can have a tensor of a full brick group.
        // Again note we always need 16 channels in this case as the first two brick are full.
        // If we have more than four bricks of elements then we add brick groups behind the first one (i.e. stacking
        // along depth). The number of channels in the final brick group may be less than 16 if there is less
        // than a full bricks worth of elements in that final brick group.
        const uint32_t numPatches = utils::DivRoundUp(numElements, patchWidth * patchHeight);
        const uint32_t reinterpretedWidth =
            numPatches <= brickGroupChannels * patchesPerBrickGroupHeight ? patchWidth : brickGroupWidth;
        const uint32_t reinterpretedHeight = numPatches <= brickGroupChannels ? patchHeight : brickGroupHeight;
        const uint32_t numFullBrickGroups  = numPatches / patchesPerBrickGroup;
        const uint32_t reinterpretedChannels =
            brickGroupChannels * numFullBrickGroups + std::min(brickGroupChannels, numPatches % patchesPerBrickGroup);
        return TensorShape{ 1, reinterpretedHeight, reinterpretedWidth, reinterpretedChannels };
    };

    const TensorShape reinterpretedInput = GetShapeContainingLinearElements(
        m_Capabilities.GetBrickGroupShape(), fullyConnected.GetInput(0).GetTensorInfo().m_Dimensions[3]);
    ReinterpretNode* reinterpretNode = m_Graph.CreateAndAddNode<ReinterpretNode>(
        reinterpretedInput, fullyConnected.GetInput(0).GetTensorInfo().m_QuantizationInfo, CompilerDataFormat::NHWCB,
        std::set<uint32_t>{ fullyConnected.GetId() });
    nodes.push_back(reinterpretNode);

    // The weight encoder for fully connected requires the input channel to be a multiple of 1024.
    // It is easier to make this adjustment here rather than the WeightEncoder itself, even though
    // it is less desirable.
    TensorInfo weightsInfo      = fullyConnected.GetWeights().GetTensorInfo();
    weightsInfo.m_Dimensions[2] = utils::RoundUpToNearestMultiple(weightsInfo.m_Dimensions[2], 1024);
    std::vector<uint8_t> paddedWeightsData =
        Pad(fullyConnected.GetWeights().GetDataVector(), utils::TotalSizeBytes(weightsInfo),
            static_cast<uint8_t>(weightsInfo.m_QuantizationInfo.m_ZeroPoint));

    Node* fcNode = m_Graph.CreateAndAddNode<MceOperationNode>(
        fullyConnected.GetInput(0).GetTensorInfo().m_Dimensions,
        fullyConnected.GetOutput(0).GetTensorInfo().m_Dimensions,
        fullyConnected.GetOutput(0).GetTensorInfo().m_QuantizationInfo, weightsInfo, paddedWeightsData,
        fullyConnected.GetBias().GetTensorInfo(), fullyConnected.GetBias().GetDataVectorAs<int32_t>(), Stride(), 1, 0,
        0, command_stream::MceOperation::FULLY_CONNECTED, CompilerDataFormat::NHWCB,
        std::set<uint32_t>{ fullyConnected.GetId() });
    nodes.push_back(fcNode);

    ConnectNodeChain(fullyConnected, nodes);
}

void NetworkToGraphConverter::Visit(Addition& addition)
{
    const auto& inputInfo0 = addition.GetInput(0).GetTensorInfo();
    const auto& inputInfo1 = addition.GetInput(1).GetTensorInfo();
    const auto& outputInfo = addition.GetOutput(0).GetTensorInfo();

    const QuantizationInfo& quantInfoInput0 = inputInfo0.m_QuantizationInfo;
    const QuantizationInfo& quantInfoInput1 = inputInfo1.m_QuantizationInfo;
    const QuantizationInfo& quantInfoOutput = outputInfo.m_QuantizationInfo;

    const SupportedLevel supportedLevel = IsAdditionSupported(inputInfo0, inputInfo1, quantInfoOutput);
    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outputInfo.m_Dimensions, quantInfoOutput,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ addition.GetId() });
        ConnectNode(addition, n);
        return;
    }

    bool isQuantInfoIdentical = (quantInfoInput0 == quantInfoInput1) && (quantInfoInput0 == quantInfoOutput);

    // use non-scaling PLE kernel if all quant info is identical for both inputs and output
    command_stream::PleOperation pleOp =
        isQuantInfoIdentical ? command_stream::PleOperation::ADDITION : command_stream::PleOperation::ADDITION_RESCALE;

    Node* n = m_Graph.CreateAndAddNode<StandalonePleOperationNode>(
        addition.GetOutput(0).GetTensorInfo().m_Dimensions, addition.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
        pleOp, CompilerDataFormat::NHWCB, std::set<uint32_t>{ addition.GetId() });
    ConnectNode(addition, n);
}

void NetworkToGraphConverter::Visit(Concatenation& concatenation)
{
    size_t numInputs = concatenation.GetInputs().size();
    uint32_t axis    = concatenation.GetConcatenationInfo().m_Axis;

    {
        std::vector<TensorInfo> inputInfos;
        inputInfos.reserve(numInputs);
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            inputInfos.push_back(concatenation.GetInput(i).GetTensorInfo());
        }

        const SupportedLevel supportedLevel =
            IsConcatenationSupported(inputInfos, concatenation.GetConcatenationInfo());

        // Currently we don't support shared inputs to concatentation
        // e.g. The output of a convolution connected to concatentation and an addition
        for (auto it : concatenation.GetInputs())
        {
            // We should still be able to estimate it, so only throw if we aren't in estimation mode.
            if (it->GetConsumers().size() > 1 && !m_EstimationMode)
            {
                throw NotSupportedException("Inputs to Concatentation cannot be connected to multiple operations");
            }
        }

        if (supportedLevel == SupportedLevel::EstimateOnly)
        {
            const auto& outInfo = concatenation.GetOutput(0).GetTensorInfo();
            Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                                 CompilerDataFormat::NHWCB,
                                                                 std::set<uint32_t>{ concatenation.GetId() });
            ConnectNode(concatenation, n);
            return;
        }
    }

    // Figure out if we need to use NHWC or if we can get away with NHWCB (which should be more efficient).
    // We can use NHWCB if the dimensions along the concat axis are all multiples of the brick group size, so
    // that the DMA is capable of placing the tensors correctly in DRAM.
    CompilerDataFormat format = CompilerDataFormat::NHWCB;
    for (uint32_t i = 0; i < numInputs; ++i)
    {
        if (concatenation.GetInput(i).GetTensorInfo().m_Dimensions[axis] % m_Capabilities.GetBrickGroupShape()[axis] !=
            0)
        {
            format = CompilerDataFormat::NHWC;
            break;
        }
    }

    Node* n = m_Graph.CreateAndAddNode<ConcatNode>(concatenation.GetOutput(0).GetTensorInfo().m_Dimensions,
                                                   concatenation.GetConcatenationInfo().m_OutputQuantizationInfo,
                                                   format, axis, std::set<uint32_t>{ concatenation.GetId() });

    ConnectNode(concatenation, n);

    // If input are not in the required format then add FormatConversionNodes to them
    std::vector<std::pair<Edge*, Node*>> edgeToAddConversion;
    for (uint32_t i = 0; i < numInputs; ++i)
    {
        if (n->GetInputFormat(i) != format)
        {
            Node* reformat = m_Graph.CreateAndAddNode<FormatConversionNode>(
                concatenation.GetInput(i).GetTensorInfo().m_Dimensions,
                concatenation.GetInput(i).GetTensorInfo().m_QuantizationInfo, format,
                std::set<uint32_t>{ concatenation.GetId() });
            edgeToAddConversion.push_back({ n->GetInput(i), reformat });
        }
    }
    for (uint32_t i = 0; i < edgeToAddConversion.size(); ++i)
    {
        m_Graph.SplitEdge(edgeToAddConversion[i].first, edgeToAddConversion[i].second);
    }

    // Our ConcatNode assumes the same quantisation info for all inputs and the output
    // we must add requantise nodes on any inputs that are different to the output.
    QuantizationInfo outputQuantInfo = concatenation.GetOutput(0).GetTensorInfo().m_QuantizationInfo;
    std::vector<std::pair<Edge*, Node*>> edgeToAddRequantize;
    for (uint32_t i = 0; i < numInputs; ++i)
    {
        if (n->GetInputQuantizationInfo(i) != outputQuantInfo)
        {
            Node* requant = m_Graph.CreateAndAddNode<RequantizeNode>(
                concatenation.GetInput(i).GetTensorInfo().m_Dimensions, outputQuantInfo, format,
                std::set<uint32_t>{ concatenation.GetId() });
            edgeToAddRequantize.push_back({ n->GetInput(i), requant });
        }
    }
    for (uint32_t i = 0; i < edgeToAddRequantize.size(); ++i)
    {
        m_Graph.SplitEdge(edgeToAddRequantize[i].first, edgeToAddRequantize[i].second);
    }
}

void NetworkToGraphConverter::Visit(Split& split)
{
    TensorInfo inputTensorInfo = split.GetInput(0).GetTensorInfo();
    const SplitInfo& splitInfo = split.GetSplitInfo();

    {
        const SupportedLevel supportedLevel = IsSplitSupported(inputTensorInfo, splitInfo);
        if (supportedLevel == SupportedLevel::EstimateOnly)
        {
            const auto& input = split.GetInput(0);
            Node* inputNode   = m_OperandToNode[&input];
            for (const auto& it : split.GetOutputs())
            {
                const TensorInfo& tensorInfo       = it.GetTensorInfo();
                EstimateOnlyNode* estimateOnlyNode = m_Graph.CreateAndAddNode<EstimateOnlyNode>(
                    tensorInfo.m_Dimensions, tensorInfo.m_QuantizationInfo, CompilerDataFormat::NHWCB,
                    std::set<uint32_t>{ split.GetId() });

                m_OperandToNode[&it] = estimateOnlyNode;
                m_Graph.Connect(inputNode, estimateOnlyNode);
            }
            return;
        }
    }

    // Figure out if we need to use NHWC or if we can get away with NHWCB (which should be more efficient).
    // We can use NHWCB if the dimensions along the split axis are all multiples of the brick group size, so
    // that the DMA is capable of extracting the tensors correctly from DRAM.
    CompilerDataFormat format = CompilerDataFormat::NHWCB;
    for (uint32_t i = 0; i < split.GetOutputs().size(); ++i)
    {
        if (split.GetOutput(i).GetTensorInfo().m_Dimensions[splitInfo.m_Axis] %
                m_Capabilities.GetBrickGroupShape()[splitInfo.m_Axis] !=
            0)
        {
            format = CompilerDataFormat::NHWC;
            break;
        }
    }

    // If our input is not in the required format then add a FormatConversion node
    Node* inputNode = m_OperandToNode[&split.GetInput(0)];
    if (inputNode->GetFormat() != format)
    {
        FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
            inputTensorInfo.m_Dimensions, inputTensorInfo.m_QuantizationInfo, format,
            std::set<uint32_t>{ split.GetId() });
        m_Graph.Connect(inputNode, conversionNode);
        inputNode = conversionNode;
    }

    // Create an ExtractSubtensor node for each output
    std::vector<Node*> extractSubtensorNodes;
    TensorShape supertensorOffset = { 0, 0, 0, 0 };
    for (uint32_t outputIdx = 0; outputIdx < split.GetOutputs().size(); outputIdx++)
    {
        TensorShape outputShape       = inputTensorInfo.m_Dimensions;
        outputShape[splitInfo.m_Axis] = splitInfo.m_Sizes[outputIdx];
        extractSubtensorNodes.push_back(m_Graph.CreateAndAddNode<ExtractSubtensorNode>(
            supertensorOffset, outputShape, inputTensorInfo.m_QuantizationInfo, format,
            std::set<uint32_t>{ split.GetId() }));
        supertensorOffset[splitInfo.m_Axis] += splitInfo.m_Sizes[outputIdx];
    }

    for (uint32_t outputIdx = 0; outputIdx < split.GetOutputs().size(); ++outputIdx)
    {
        m_Graph.Connect(inputNode, extractSubtensorNodes[outputIdx]);
        m_OperandToNode[&split.GetOutput(outputIdx)] = extractSubtensorNodes[outputIdx];
    }
}

void NetworkToGraphConverter::Visit(Constant& constant)
{
    Node* constantNode = m_Graph.CreateAndAddNode<ConstantNode>(constant.GetTensorInfo(), constant.GetDataVector(),
                                                                std::set<uint32_t>{ constant.GetId() });

    ConnectNode(constant, constantNode);
}

void NetworkToGraphConverter::Visit(DepthwiseConvolution& depthwiseConvolution)
{
    std::vector<Node*> nodes;

    const SupportedLevel supportedLevel = IsDepthwiseConvolutionSupported(
        depthwiseConvolution.GetBias().GetTensorInfo(), depthwiseConvolution.GetWeights().GetTensorInfo(),
        depthwiseConvolution.GetConvolutionInfo(), depthwiseConvolution.GetInput(0).GetTensorInfo());

    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        const auto& outInfo = depthwiseConvolution.GetOutput(0).GetTensorInfo();
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ depthwiseConvolution.GetId() });
        ConnectNode(depthwiseConvolution, n);
        return;
    }

    const ConvolutionInfo& convInfo = depthwiseConvolution.GetConvolutionInfo();

    if (convInfo.m_Stride.m_X > 1 || convInfo.m_Stride.m_Y > 1)
    {
        // Create additional layer before strided convolution
        // Only supports stride 2x2 for now
        assert(convInfo.m_Stride.m_X == 2 && convInfo.m_Stride.m_Y == 2);

        uint32_t h =
            utils::DivRoundUp(depthwiseConvolution.GetInput(0).GetTensorInfo().m_Dimensions[1], convInfo.m_Stride.m_Y);
        uint32_t w =
            utils::DivRoundUp(depthwiseConvolution.GetInput(0).GetTensorInfo().m_Dimensions[2], convInfo.m_Stride.m_X);
        uint32_t c = utils::GetNumSubmapChannels(depthwiseConvolution.GetInput(0).GetTensorInfo().m_Dimensions[3],
                                                 convInfo.m_Stride.m_X, convInfo.m_Stride.m_Y, m_Capabilities);

        TensorInfo interleaveOutput =
            TensorInfo({ depthwiseConvolution.GetInput(0).GetTensorInfo().m_Dimensions[0], h, w, c },
                       depthwiseConvolution.GetInput(0).GetTensorInfo().m_DataType,
                       depthwiseConvolution.GetInput(0).GetTensorInfo().m_DataFormat,
                       depthwiseConvolution.GetInput(0).GetTensorInfo().m_QuantizationInfo);

        Node* interleaveNode = m_Graph.CreateAndAddNode<FuseOnlyPleOperationNode>(
            interleaveOutput.m_Dimensions, interleaveOutput.m_QuantizationInfo,
            command_stream::PleOperation::INTERLEAVE_2X2_2_2, CompilerDataFormat::NHWCB,
            ShapeMultiplier{ { 1, convInfo.m_Stride.m_Y },
                             { 1, convInfo.m_Stride.m_X },
                             { convInfo.m_Stride.m_X * convInfo.m_Stride.m_Y } },
            std::set<uint32_t>{ depthwiseConvolution.GetId() });
        nodes.push_back(interleaveNode);
    }

    // We support channel multiplier > 1 if there is only 1 input channel because
    // A depthwise convolution with 1 input channel is equivalent to a normal convolution
    command_stream::MceOperation operation;
    TensorInfo weightInfo;
    if (depthwiseConvolution.GetWeights().GetTensorInfo().m_Dimensions[3] > 1)
    {
        assert(depthwiseConvolution.GetWeights().GetTensorInfo().m_Dimensions[2] == 1);
        weightInfo              = depthwiseConvolution.GetWeights().GetTensorInfo();
        weightInfo.m_DataFormat = DataFormat::HWIO;
        operation               = command_stream::MceOperation::CONVOLUTION;
    }
    else
    {
        weightInfo = depthwiseConvolution.GetWeights().GetTensorInfo();
        operation  = command_stream::MceOperation::DEPTHWISE_CONVOLUTION;
    }
    // We don't use winograd for depthwise convolution
    Node* convNode = m_Graph.CreateAndAddNode<MceOperationNode>(
        depthwiseConvolution.GetInput(0).GetTensorInfo().m_Dimensions,
        depthwiseConvolution.GetOutput(0).GetTensorInfo().m_Dimensions,
        depthwiseConvolution.GetOutput(0).GetTensorInfo().m_QuantizationInfo, weightInfo,
        depthwiseConvolution.GetWeights().GetDataVector(), depthwiseConvolution.GetBias().GetTensorInfo(),
        depthwiseConvolution.GetBias().GetDataVectorAs<int32_t>(), depthwiseConvolution.GetConvolutionInfo().m_Stride,
        1, depthwiseConvolution.GetConvolutionInfo().m_Padding.m_Top,
        depthwiseConvolution.GetConvolutionInfo().m_Padding.m_Left, operation, CompilerDataFormat::NHWCB,
        std::set<uint32_t>{ depthwiseConvolution.GetId() });
    nodes.push_back(convNode);

    ConnectNodeChain(depthwiseConvolution, nodes);
}

void NetworkToGraphConverter::Visit(Convolution& convolution)
{
    std::vector<Node*> nodes;

    const SupportedLevel supportedLevel =
        IsConvolutionSupported(convolution.GetBias().GetTensorInfo(), convolution.GetWeights().GetTensorInfo(),
                               convolution.GetConvolutionInfo(), convolution.GetInput(0).GetTensorInfo());

    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        const auto& outInfo = convolution.GetOutput(0).GetTensorInfo();
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ convolution.GetId() });
        ConnectNode(convolution, n);
        return;
    }

    const ConvolutionInfo& convInfo = convolution.GetConvolutionInfo();
    if (convInfo.m_Stride.m_X > 1 || convInfo.m_Stride.m_Y > 1)
    {
        // Create additional layer before strided convolution
        // Only supports stride 2x2 for now.
        // Winograd is not considered for strided convolution.
        assert(convInfo.m_Stride.m_X == 2 && convInfo.m_Stride.m_Y == 2);

        uint32_t h = utils::DivRoundUp(convolution.GetInput(0).GetTensorInfo().m_Dimensions[1], convInfo.m_Stride.m_Y);
        uint32_t w = utils::DivRoundUp(convolution.GetInput(0).GetTensorInfo().m_Dimensions[2], convInfo.m_Stride.m_X);
        uint32_t c = utils::GetNumSubmapChannels(convolution.GetInput(0).GetTensorInfo().m_Dimensions[3],
                                                 convInfo.m_Stride.m_X, convInfo.m_Stride.m_Y, m_Capabilities);

        TensorInfo interleaveOutput = TensorInfo({ convolution.GetInput(0).GetTensorInfo().m_Dimensions[0], h, w, c },
                                                 convolution.GetInput(0).GetTensorInfo().m_DataType,
                                                 convolution.GetInput(0).GetTensorInfo().m_DataFormat,
                                                 convolution.GetInput(0).GetTensorInfo().m_QuantizationInfo);

        Node* interleaveNode = m_Graph.CreateAndAddNode<FuseOnlyPleOperationNode>(
            interleaveOutput.m_Dimensions, interleaveOutput.m_QuantizationInfo,
            command_stream::PleOperation::INTERLEAVE_2X2_2_2, CompilerDataFormat::NHWCB,
            ShapeMultiplier{ { 1, convInfo.m_Stride.m_Y },
                             { 1, convInfo.m_Stride.m_X },
                             { convInfo.m_Stride.m_X * convInfo.m_Stride.m_Y } },
            std::set<uint32_t>{ convolution.GetId() });
        nodes.push_back(interleaveNode);
    }

    Node* convNode = m_Graph.CreateAndAddNode<MceOperationNode>(
        convolution.GetInput(0).GetTensorInfo().m_Dimensions, convolution.GetOutput(0).GetTensorInfo().m_Dimensions,
        convolution.GetOutput(0).GetTensorInfo().m_QuantizationInfo, convolution.GetWeights().GetTensorInfo(),
        convolution.GetWeights().GetDataVector(), convolution.GetBias().GetTensorInfo(),
        convolution.GetBias().GetDataVectorAs<int32_t>(), convolution.GetConvolutionInfo().m_Stride, 1,
        convolution.GetConvolutionInfo().m_Padding.m_Top, convolution.GetConvolutionInfo().m_Padding.m_Left,
        command_stream::MceOperation::CONVOLUTION, CompilerDataFormat::NHWCB,
        std::set<uint32_t>{ convolution.GetId() });
    nodes.push_back(convNode);

    ConnectNodeChain(convolution, nodes);
}

void NetworkToGraphConverter::Visit(TransposeConvolution& transposeConvolution)
{
    const Stride& stride                    = transposeConvolution.GetConvolutionInfo().m_Stride;
    const TensorInfo& weightsInfo           = transposeConvolution.GetWeights().GetTensorInfo();
    const std::vector<uint8_t>& weightsData = transposeConvolution.GetWeights().GetDataVector();
    const TensorInfo& biasInfo              = transposeConvolution.GetBias().GetTensorInfo();
    std::vector<int32_t> biasData           = transposeConvolution.GetBias().GetDataVectorAs<int32_t>();
    const Padding& padding                  = transposeConvolution.GetConvolutionInfo().m_Padding;
    const TensorInfo& inputInfo             = transposeConvolution.GetInput(0).GetTensorInfo();
    const TensorInfo& outputInfo            = transposeConvolution.GetOutput(0).GetTensorInfo();

    const SupportedLevel supportedLevel = IsTransposeConvolutionSupported(
        transposeConvolution.GetBias().GetTensorInfo(), transposeConvolution.GetWeights().GetTensorInfo(),
        transposeConvolution.GetConvolutionInfo(), transposeConvolution.GetInput(0).GetTensorInfo());

    if (supportedLevel == SupportedLevel::EstimateOnly)
    {
        const auto& outInfo = transposeConvolution.GetOutput(0).GetTensorInfo();
        Node* n = m_Graph.CreateAndAddNode<EstimateOnlyNode>(outInfo.m_Dimensions, outInfo.m_QuantizationInfo,
                                                             CompilerDataFormat::NHWCB,
                                                             std::set<uint32_t>{ transposeConvolution.GetId() });
        ConnectNode(transposeConvolution, n);
        return;
    }

    std::vector<Node*> transposeConvNodes =
        CreateTransposeConv(m_Graph, stride, weightsInfo, weightsData, biasInfo, std::move(biasData), padding,
                            inputInfo, outputInfo, transposeConvolution.GetId());

    ConnectNodeChain(transposeConvolution, transposeConvNodes);
}

void NetworkToGraphConverter::Visit(Output& output)
{
    std::vector<Node*> nodes;

    // Add conversion node if necessary
    if (m_OperandToNode[&output.GetInput(0)]->GetFormat() !=
        ConvertExternalToCompilerDataFormat(output.GetTensorInfo().m_DataFormat))
    {
        FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
            output.GetTensorInfo().m_Dimensions, output.GetTensorInfo().m_QuantizationInfo,
            ConvertExternalToCompilerDataFormat(output.GetTensorInfo().m_DataFormat),
            std::set<uint32_t>{ output.GetInput(0).GetProducer().GetId() });
        nodes.push_back(conversionNode);
    }

    // Note that we return the ID of the *producer* that feeds in to the output node, not the ID of the output
    // node itself. This is for consistency when we start splitting the network and need to identify network outputs
    // that do not have their own unique node. See documentation on InputBufferInfo struct in Support.hpp for details.
    Node* outputNode = m_Graph.CreateAndAddNode<OutputNode>(
        std::set<uint32_t>{ output.GetInput(0).GetProducer().GetId() }, output.GetInput(0).GetProducerOutputIndex());
    nodes.push_back(outputNode);

    ConnectNodeChain(output, nodes);
}

void NetworkToGraphConverter::Visit(Input& input)
{
    std::vector<Node*> nodes;
    Node* n = m_Graph.CreateAndAddNode<InputNode>(input.GetTensorInfo(), std::set<uint32_t>{ input.GetId() });
    nodes.push_back(n);

    // Add a format conversion to NHWCB if needed because operations work best with NHWCB.
    if (n->GetFormat() != CompilerDataFormat::NHWCB)
    {
        FormatConversionNode* conversionNode = m_Graph.CreateAndAddNode<FormatConversionNode>(
            input.GetOutput(0).GetTensorInfo().m_Dimensions, input.GetOutput(0).GetTensorInfo().m_QuantizationInfo,
            CompilerDataFormat::NHWCB, std::set<uint32_t>{ input.GetId() });
        nodes.push_back(conversionNode);
    }
    ConnectNodeChain(input, nodes);
}

void NetworkToGraphConverter::Visit(DepthToSpace& depthToSpace)
{
    // We implement depth-to-space (block-size 2) with a transpose convolution (stride 2) with a 2x2 kernel,
    // where the weights are used to 'select' which elements of the input are placed into each element of the output.
    // By setting the stride and kernel size the same, the output is made by multiplying the kernel by each IFM (x, y)
    // position and tiling the resulting tensors.
    // The weight vector along input-channels at each (u, v) position in the kernel will be dotted with the IFM along
    // channels at each (x, y) position.
    // This means that we can choose different weight vectors to be dotted with the IFM vectors for each of the four
    // output pixels that we want to derive from each input pixel, so that we can select the correct IFM channel for each.
    // The weight vectors at each (u, v) are therefore simple "one-hot" vectors.
    // Below is an example for a 1x1x4 input being turned into a 2x2x1 output.
    //
    //  Input:                     Output:                       Weights:
    // (with padding)
    //
    //  Channel 0:                Channel 0:                  Input channel 0:
    //     I0                       I0   I1                        1   0
    //                              I2   I3                        0   0
    //
    //  Channel 1:                                            Input channel 1:
    //     I1                                                      0   1
    //                                                             0   0
    //
    //  Channel 2:                                            Input channel 2:
    //     I2                                                      0   0
    //                                                             1   0
    //
    //  Channel 3:                                            Input channel 3:
    //     I3                                                      0   0
    //                                                             0   1
    //
    uint32_t blockSize = depthToSpace.GetDepthToSpaceInfo().m_BlockSize;
    assert(blockSize == 2);    // Checked by IsDepthToSpaceSupported
    uint32_t ifmChannelsPerOfm = blockSize * blockSize;

    const TensorShape& inputShape  = depthToSpace.GetInput(0).GetTensorInfo().m_Dimensions;
    const TensorShape& outputShape = depthToSpace.GetOutput(0).GetTensorInfo().m_Dimensions;

    // Set weights according to the above explanation
    const float weightsScale = 0.5f;    // We can't use a scale of 1.0 as that would cause an overall multiplier >= 1.
    TensorInfo weightsInfo({ blockSize, blockSize, inputShape[3], outputShape[3] }, DataType::UINT8_QUANTIZED,
                           DataFormat::HWIO, QuantizationInfo(0, weightsScale));
    std::vector<uint8_t> weightsData(utils::GetNumElements(weightsInfo.m_Dimensions), 0);
    utils::TensorData weights(weightsData.data(), weightsInfo.m_Dimensions);
    for (uint32_t ofmIdx = 0; ofmIdx < outputShape[3]; ++ofmIdx)
    {
        // Each OFM is derived from 4 IFMs which are distributed across the channels.
        // All of the top-left elements come first, then all the top-right, bottom-left then finally bottom-right.
        // This means that the IFMs for a particular OFM start at the same index as the OFM
        // and are separated from each other by the number of blocks.
        const uint32_t ifmBase   = ofmIdx;
        const uint32_t ifmStride = inputShape[3] / ifmChannelsPerOfm;
        // Set the weight vectors for each of the (u, v) positions, each of which will contain just one non-zero value
        for (uint32_t v = 0; v < blockSize; ++v)
        {
            for (uint32_t u = 0; u < blockSize; ++u)
            {
                // Calculate which IFM we want this weight vector to select
                const uint32_t ifmWithinBlock = v * blockSize + u;
                const uint32_t ifmIdx         = ifmBase + ifmWithinBlock * ifmStride;
                weights.SetElement(v, u, ifmIdx, ofmIdx, static_cast<uint8_t>(1.0f / weightsScale));
            }
        }
    }

    // Set biases to all zero (we don't need a bias)
    const float biasScale = weightsScale * depthToSpace.GetInput(0).GetTensorInfo().m_QuantizationInfo.m_Scale;
    TensorInfo biasInfo({ 1, 1, 1, outputShape[3] }, DataType::UINT8_QUANTIZED, DataFormat::NHWC,
                        QuantizationInfo(0, biasScale));
    std::vector<int32_t> biasData(utils::GetNumElements(biasInfo.m_Dimensions), 0);

    std::vector<Node*> transposeConvNodes =
        CreateTransposeConv(m_Graph, Stride(blockSize, blockSize), weightsInfo, std::move(weightsData), biasInfo,
                            std::move(biasData), Padding(0, 0), depthToSpace.GetInput(0).GetTensorInfo(),
                            depthToSpace.GetOutput(0).GetTensorInfo(), depthToSpace.GetId());

    ConnectNodeChain(depthToSpace, transposeConvNodes);
}

void NetworkToGraphConverter::Visit(EstimateOnly& estimateOnly)
{
    // Add an EstimateOnly node for each output of the EstimateOnly operation
    for (const auto& it : estimateOnly.GetOutputs())
    {
        const TensorInfo& tensorInfo       = it.GetTensorInfo();
        EstimateOnlyNode* estimateOnlyNode = m_Graph.CreateAndAddNode<EstimateOnlyNode>(
            tensorInfo.m_Dimensions, tensorInfo.m_QuantizationInfo, CompilerDataFormat::NHWCB,
            std::set<uint32_t>{ estimateOnly.GetId() });

        m_OperandToNode[&it] = estimateOnlyNode;

        // Each output is connected to each input
        for (const auto& input : estimateOnly.GetInputs())
        {
            Node* inputNode = m_OperandToNode[input];
            m_Graph.Connect(inputNode, estimateOnlyNode);
        }
    }
}

void NetworkToGraphConverter::ConnectNode(const Operation& operation, Node* node)
{
    ConnectNodeChain(operation, { node });
}

void NetworkToGraphConverter::ConnectNodeChain(const Operation& operation, const std::vector<Node*>& nodes)
{
    // This function does not support multiple outputs as that would require knowledge of which node
    // corresponds to which output.
    assert(operation.GetOutputs().size() <= 1);

    for (uint32_t i = 0; i < static_cast<uint32_t>(nodes.size()) - 1; ++i)
    {
        m_Graph.Connect(nodes[i], nodes[i + 1]);
    }

    for (const Operand* op : operation.GetInputs())
    {
        m_Graph.Connect(m_OperandToNode.at(op), nodes.front());
    }

    if (operation.GetOutputs().size() > 0)
    {
        m_OperandToNode[&operation.GetOutput(0)] = nodes.back();
    }
}

}    // namespace support_library

}    // namespace ethosn
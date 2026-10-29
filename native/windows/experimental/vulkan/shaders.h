#include <vulkan/spirv.h>
#include <initializer_list>
static void instruction(std::vector<uint32_t>&s,SpvOp op,std::initializer_list<uint32_t>args){s.push_back((uint32_t(args.size()+1)<<16)|op);s.insert(s.end(),args.begin(),args.end());}
static uint32_t float_bits(float x){uint32_t bits;std::memcpy(&bits,&x,4);return bits;}
static std::vector<uint32_t> vertex_spirv(){
 std::vector<uint32_t>s={SpvMagicNumber,0x10000,0,23,0};
 instruction(s,SpvOpCapability,{SpvCapabilityShader});instruction(s,SpvOpMemoryModel,{SpvAddressingModelLogical,SpvMemoryModelGLSL450});
 instruction(s,SpvOpEntryPoint,{SpvExecutionModelVertex,1,0x6e69616d,0,12,13});
 instruction(s,SpvOpDecorate,{12,SpvDecorationLocation,0});instruction(s,SpvOpDecorate,{9,SpvDecorationBlock});instruction(s,SpvOpMemberDecorate,{9,0,SpvDecorationBuiltIn,SpvBuiltInPosition});
 instruction(s,SpvOpTypeVoid,{3});instruction(s,SpvOpTypeFunction,{4,3});instruction(s,SpvOpTypeFloat,{5,32});instruction(s,SpvOpTypeVector,{6,5,2});instruction(s,SpvOpTypeVector,{7,5,4});
 instruction(s,SpvOpTypePointer,{8,SpvStorageClassInput,6});instruction(s,SpvOpTypeStruct,{9,7});instruction(s,SpvOpTypePointer,{10,SpvStorageClassOutput,9});instruction(s,SpvOpTypePointer,{11,SpvStorageClassOutput,7});
 instruction(s,SpvOpVariable,{8,12,SpvStorageClassInput});instruction(s,SpvOpVariable,{10,13,SpvStorageClassOutput});instruction(s,SpvOpConstant,{5,14,float_bits(0)});instruction(s,SpvOpConstant,{5,15,float_bits(1)});instruction(s,SpvOpTypeInt,{16,32,0});instruction(s,SpvOpConstant,{16,17,0});
 instruction(s,SpvOpFunction,{3,1,SpvFunctionControlMaskNone,4});instruction(s,SpvOpLabel,{2});instruction(s,SpvOpLoad,{6,18,12});instruction(s,SpvOpCompositeExtract,{5,19,18,0});instruction(s,SpvOpCompositeExtract,{5,20,18,1});instruction(s,SpvOpCompositeConstruct,{7,21,19,20,14,15});instruction(s,SpvOpAccessChain,{11,22,13,17});instruction(s,SpvOpStore,{22,21});instruction(s,SpvOpReturn,{});instruction(s,SpvOpFunctionEnd,{});return s;
}
static std::vector<uint32_t> fragment_spirv(){
 std::vector<uint32_t>s={SpvMagicNumber,0x10000,0,14,0};
 instruction(s,SpvOpCapability,{SpvCapabilityShader});instruction(s,SpvOpMemoryModel,{SpvAddressingModelLogical,SpvMemoryModelGLSL450});instruction(s,SpvOpEntryPoint,{SpvExecutionModelFragment,1,0x6e69616d,0,8});instruction(s,SpvOpExecutionMode,{1,SpvExecutionModeOriginUpperLeft});instruction(s,SpvOpDecorate,{8,SpvDecorationLocation,0});
 instruction(s,SpvOpTypeVoid,{3});instruction(s,SpvOpTypeFunction,{4,3});instruction(s,SpvOpTypeFloat,{5,32});instruction(s,SpvOpTypeVector,{6,5,4});instruction(s,SpvOpTypePointer,{7,SpvStorageClassOutput,6});instruction(s,SpvOpVariable,{7,8,SpvStorageClassOutput});
 instruction(s,SpvOpConstant,{5,9,float_bits(.2f)});instruction(s,SpvOpConstant,{5,10,float_bits(.6f)});instruction(s,SpvOpConstant,{5,11,float_bits(.9f)});instruction(s,SpvOpConstant,{5,12,float_bits(1)});instruction(s,SpvOpConstantComposite,{6,13,9,10,11,12});
 instruction(s,SpvOpFunction,{3,1,SpvFunctionControlMaskNone,4});instruction(s,SpvOpLabel,{2});instruction(s,SpvOpStore,{8,13});instruction(s,SpvOpReturn,{});instruction(s,SpvOpFunctionEnd,{});return s;
}

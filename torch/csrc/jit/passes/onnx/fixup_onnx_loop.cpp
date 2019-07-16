#include <torch/csrc/jit/passes/onnx/fixup_onnx_loop.h>

namespace torch {
namespace jit {

namespace onnx{
using namespace ::c10::onnx;
}

Node* CreateCastToBoolNode(Value* val, Graph* graph) {
  Node* cast_node = graph->create(onnx::Cast);
  cast_node->addInput(val);
  cast_node->i_(attr::to, /*Bool*/9);
  return cast_node;
}

Node* InsertCastForCond(Value* cond_val, Graph* graph, Node* consumer_node) {
  // prev:  cond_val -> consumer_node
  // after: cond_val -> cast -> consumer_node
  // NOTE: The cast is required because operators like PyTorch Greater/Less
  //       return tensor in type torch.uint8. However the type for condition
  //       input in ONNX Loop must be bool.
  Node* cast_node = CreateCastToBoolNode(cond_val, graph);
  cast_node->insertBefore(consumer_node);

  consumer_node->replaceInputWith(cond_val, cast_node->output());
  return cast_node;
}

bool IsCondCastRequired(Value* cond_val) {
  const auto& type = cond_val->type();
  if (type->isSubclass(TypeKind::DimensionedTensorType)) {
    return type->expect<DimensionedTensorType>()->scalarType() != c10::kBool;
  }
  return !type->isSubclass(TypeKind::BoolType);
}

void FixupONNXLoops(Block* block) {
  for (auto* node : block->nodes()) {
    if (node->kind() == ::c10::onnx::Loop) {
      auto* loop_node = node;
      auto* graph = loop_node->owningGraph();

      // add cast to condition input outside the loop.
      Value* cond_val = loop_node->inputs()[1];
      if (IsCondCastRequired(cond_val))
        InsertCastForCond(cond_val, graph, loop_node);

      // Setup Loop input cond and i.
      TORCH_INTERNAL_ASSERT(loop_node->blocks().size() == 1);
      auto* sub_block = loop_node->blocks()[0];
      Value* cond = sub_block->insertInput(1, "cond");
      cond->setType(BoolType::create());

      Value* i = sub_block->inputs()[0];
      i->setType(CompleteTensorType::fromNumberType(IntType::get()));

      // set type for the rest of sub_block's inputs
      for (size_t i = 2; i<sub_block->inputs().size(); ++i) {
        sub_block->inputs()[i]->setType(loop_node->inputs()[i]->type());
      }

      // add cast to condition input inside the loop.
      Value* next_cond_val = sub_block->outputs()[0];
      if (IsCondCastRequired(next_cond_val))
        InsertCastForCond(next_cond_val, graph, sub_block->return_node());
    }
    else if (node->kind() == ::c10::onnx::If) {
      auto* if_node = node;
      auto* graph = if_node->owningGraph();

      // add cast to condition input outside the loop.
      Value* cond_val = if_node->inputs()[0];
      if (IsCondCastRequired(cond_val))
        InsertCastForCond(cond_val, graph, if_node);
    }
    for (Block* block : node->blocks()) {
      FixupONNXLoops(block);
    }
  }
}

void FixupONNXLoops(std::shared_ptr<Graph>& graph) {
  FixupONNXLoops(graph->block());
}

} // namespace jit
} // namespace torch

//! Deterministic node graph data model, compiler, evaluation VM, and wait-free swap slot.

pub mod builtins;
pub mod compiler;
pub mod eval;
pub mod model;
pub mod plan_slot;

pub use compiler::{CompiledGraphPlan, LatchedBinding, compile_graph_document};
pub use eval::{HostEvalContext, LatchedBridge, SafeGraphInputs};
pub use model::{GraphDocument, GraphEdge, GraphNode, PortDecl, PortDirection, ValueType};
pub use plan_slot::PlanExchange;

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/relay/collage/partition_rule.h
 * \brief Compositional partitioning rules.
 */

#ifndef TVM_RELAY_COLLAGE_PARTITION_RULE_H_
#define TVM_RELAY_COLLAGE_PARTITION_RULE_H_

#include <tvm/relay/dataflow_pattern.h>
#include <tvm/relay/expr.h>

#include <string>
#include <vector>

#include "../../printer/doc.h"
#include "./candidate_partition.h"
#include "./sub_graph.h"

namespace tvm {
namespace relay {
namespace collage {

/*!
 * \brief Type of function to check if a matched sub-expression should be accepted by a rule. This
 * can be used to, eg, reject operators of unsupported shape or dtype, or otherwise implement rules
 * which are difficult to express in the dataflow pattern language directly.
 */
using TPatternPredicate = TypedPackedFunc<bool(const Expr& matched_sub_expr)>;

/*!
 * \brief The default pattern predicate. Always returns true.
 */
bool DefaultPatternPredicate(const Expr& matched_sub_expr);

/*!
 * \brief Base class of all partition rules.
 *
 * A \p PartitionRule describes how to find a set of \p CandidatePartitions for a \p DataflowGraph.
 * The candidates are allowed to overlap, and ultimately it is the job of the Collage searcher to
 * find a selection of candidates which covers the whole Relay expression without overlap. Partition
 * rules are paired with their \p Target and other 'top level' configuration in a \p PartitionSpec.
 *
 * We provide a set of 'base' partition rules which produce candidates from the dataflow graph
 * directly. We also provide a set of 'combinator' partition rules which can produce new candidates
 * from the results of an arbitrary sub-rule or sub-rules. By mixing these base and combinator
 * rules we can express a wide variety of partition strategies and encoding conventions.
 *
 * There may be many thousands of candidates in flight during the Collage search. We take care to
 * defer constructing or rewriting Relay expressions until absolutely necessary. We only pay for
 * extracting a function to represent a candidate when we need to measure it's cost. And we only
 * pay for rewriting the overall Relay expression to commit to a partitioning when the Collage
 * search has completed.
 *
 * The base rules implemented so far:
 *  - \p DFPatternPartitionRule: Given a \p DFPattern and expression predicate, produces a candidate
 *    for every sub-graph matched by the pattern and predicate. Unlike the \p PatternRewriter,
 *    candidates are free to overlap. Used to bring BYOC patterns into the Collage framework.
 *  - \p OpCallByKindPartitionRule: Uses the "TOpPattern" attribute provided for every Relay
 *    operator to produce a candidate for every call to a 'fusable Relay operator'. Used to
 *    look ahead to how TVM will fuse sub-graphs.
 *
 * The combinator rules implemented so far:
 *  - \p CompositePartitionRule: Indicates all candidates matched by the sub-rule should be wrapped
 *    by a "Composite" function. The "Composite" name is taken from the rule name. Used to indicate
 *    Relay operators (or groups of Relay operators) should be mapped to target-specific operators,
 *    both for BYOC and TVM external library integrations.
 *  - \p PrimitivePartitionRule: Indicates all candidates matched by the sub-rule should be wrapped
 *    by a "Primitive" function, possibly with an additional "Compiler" attribute. Used to
 *    delineate a partition (or kernel).
 *  - \p UnionPartitionRule: Simply unions all the candidates from all sub-rules together. Used to
 *    combine individual \p DFPatternPartitionRules.
 *  - \p OnlyValidPartitionRule: Given a \p SubGraphConfig, ignores candidates with 'invalid'
 *    sub-graphs. Used to limit the maximum candidate depth, the number of independent outputs,
 *    and whether intermediate 'taps' are allowed.
 *  - \p HostPartitionRule: Produces candidates for all Relay expressions which could be
 *    'left behind' for execution by the host (eg on the VM). This rule lets us simplify the
 *    overall Collage search algorithm.
 *
 * (Though not yet implemented, we'd like to allow a combinator rule which will union candidate
 * based on their 'anchor' operators. This can be used to implement 'vertical' and 'horizontal'
 * partition on more primitive candidates. Note that the \p SubGraph machinery supports
 * multiple-input and -output sub-graphs and their validation, so horizontal partition is easy
 * implement.)
 */
class PartitionRuleNode : public Object {
 public:
  /*!
   * \brief A unique (over all rules for the same target) name for the rule. Rule names are
   * combined and captured with \p PartitionCandidate rule names for debuggability and
   * explainability. Some rules will copy the rule name into function attributes.
   *
   */
  String rule_name_;

  void VisitAttrs(AttrVisitor* v);

  /*!
   * \brief Returns all the possible candidate partitions according to this rule for the overall
   * expression corresponding to \p dataflow_graph. The candidates will generally have unknown
   * target and cost: the target will be filled in by the \p PartitionSpec, while the cost will
   * be filled in lazily.
   */
  virtual std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                        const PartitionSpec& spec) const;

  std::string ToString() const;
  Doc ToDoc() const;

 protected:
  virtual void AppendBodyItems(std::vector<Doc>* body_items) const;

 public:
  static constexpr const char* _type_key = "relay.collage.PartitionRule";
  static constexpr const uint32_t _type_child_slots = 10;
  TVM_DECLARE_BASE_OBJECT_INFO(PartitionRuleNode, Object);
};

class PartitionRule : public ObjectRef {
 public:
  explicit PartitionRule(String rule_name);

  TVM_DEFINE_OBJECT_REF_METHODS(PartitionRule, ObjectRef, PartitionRuleNode);
};

/*!
 * \brief Partition rule which fires on all sub-expressions matching a dataflow-pattern and pattern
 * predicate. It is valid for matching candidates to overlap.
 */
class DFPatternPartitionRuleNode : public PartitionRuleNode {
 public:
  /*!
   * \brief Relay pattern.
   */
  DFPattern pattern_;

  /*!
   * \brief Predicate on matched sub-expression to decide if partition rule should fire.
   */
  TPatternPredicate predicate_;

  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

  static constexpr const char* _type_key = "relay.collage.DFPatternPartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(DFPatternPartitionRuleNode, PartitionRuleNode);
};

class DFPatternPartitionRule : public PartitionRule {
 public:
  DFPatternPartitionRule(String rule_name, DFPattern pattern,
                         TPatternPredicate predicate = DefaultPatternPredicate);

  TVM_DEFINE_OBJECT_REF_METHODS(DFPatternPartitionRule, PartitionRule, DFPatternPartitionRuleNode);
};

/*!
 * \brief Partition rule which wraps candidates within a function with the "Composite" attribute
 * bound to the given rule name.
 *
 * This is the standard way by which operators or operator groups are tagged as being supported
 * by a particular externally provided function. It is up to the BYOC lowering function to
 * recognize the "Composite" name and emit the appropriate code or call.
 */
class CompositePartitionRuleNode : public PartitionRuleNode {
 public:
  /*! \brief The sub-partition rule. */
  PartitionRule sub_rule_;

  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

  static constexpr const char* _type_key = "relay.collage.CompositePartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(CompositePartitionRuleNode, PartitionRuleNode);
};

class CompositePartitionRule : public PartitionRule {
 public:
  CompositePartitionRule(String rule_name, PartitionRule sub_rule);

  TVM_DEFINE_OBJECT_REF_METHODS(CompositePartitionRule, PartitionRule, CompositePartitionRuleNode);
};

/*!
 * \brief Partition rule which wraps candidates within a function with the "Primitive" attribute
 * bound to 1. If the partition spec target(s) have the "compiler" attribute then that name is
 * also added to the function as a "Compiler" attribute.
 *
 * This is the standard way by which sub-graphs are marked as being in a 'partition' who's
 * compilation will be managed by an external BYOC toolchain. It can also be used to mark
 * sub-graphs for lowering to a single kernel by the built-in TVM lowering machinery.
 */
class PrimitivePartitionRuleNode : public PartitionRuleNode {
 public:
  /*! \brief The sub-partition rule. */
  PartitionRule sub_rule_;

  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

  static constexpr const char* _type_key = "relay.collage.PrimitivePartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(PrimitivePartitionRuleNode, PartitionRuleNode);
};

class PrimitivePartitionRule : public PartitionRule {
 public:
  PrimitivePartitionRule(String rule_name, PartitionRule sub_rule);

  TVM_DEFINE_OBJECT_REF_METHODS(PrimitivePartitionRule, PartitionRule, PrimitivePartitionRuleNode);
};

/*!
 * \brief Partition rule which simply unions all matches from all sub-partition rules.
 *
 * This can be used to combine the results of a set of, eg, DFPatternPartitionRules.
 */
class UnionPartitionRuleNode : public PartitionRuleNode {
 public:
  Array<PartitionRule> sub_rules_;

  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

  static constexpr const char* _type_key = "relay.collage.UnionPartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(UnionPartitionRuleNode, PartitionRuleNode);
};

class UnionPartitionRule : public PartitionRule {
 public:
  UnionPartitionRule(String rule_name, Array<PartitionRule> sub_rules);

  TVM_DEFINE_OBJECT_REF_METHODS(UnionPartitionRule, PartitionRule, UnionPartitionRuleNode)
};

/*
 *! \brief Partition rule which places calls to Relay operators with a "TOpPattern" attribute of
 * \p kOutEWiseFusable or less in their own singleton sub-graph. No other Relay sub-expressions
 * (such as tuples or tuple projection) are selected, and it is up to outer partition rules to
 * account for them.
 */
class OpCallByKindPartitionRuleNode : public PartitionRuleNode {
 public:
  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

  static constexpr const char* _type_key = "relay.collage.OpCallByKindPartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(OpCallByKindPartitionRuleNode, PartitionRuleNode);
};

class OpCallByKindPartitionRule : public PartitionRule {
 public:
  explicit OpCallByKindPartitionRule(String rule_name);

  TVM_DEFINE_OBJECT_REF_METHODS(OpCallByKindPartitionRule, PartitionRule,
                                OpCallByKindPartitionRuleNode);
};

/*!
 * \brief Partition rules which keeps only candidates from the sub-rule whose sub-groups are valid
 * w.r.t. the given \p SubGraphConfig.
 */
class OnlyValidPartitionRuleNode : public PartitionRuleNode {
 public:
  PartitionRule sub_rule_;
  SubGraphConfig config_;

  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

 public:
  static constexpr const char* _type_key = "relay.collage.OnlyValidPartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(OnlyValidPartitionRuleNode, PartitionRuleNode);
};

class OnlyValidPartitionRule : public PartitionRule {
 public:
  OnlyValidPartitionRule(String rule_name, PartitionRule sub_rule, const SubGraphConfig& config);

  TVM_DEFINE_OBJECT_REF_METHODS(OnlyValidPartitionRule, PartitionRule, OnlyValidPartitionRuleNode);
};

/*!
 * \brief Partition rule which selects nodes which can be 'left behind' to be executed by the host
 * (eg on the VM). This includes most of the 'interstitial' Relay constructs, such a let bindings,
 * operators on references, calls to non-operator functions, and so on. It can also include the
 * construction of and projection from tuples which may not be supported within a partition.
 */
class HostPartitionRuleNode : public PartitionRuleNode {
 public:
  void VisitAttrs(AttrVisitor* v);

  std::vector<CandidatePartition> AllCandidates(const DataflowGraph& dataflow_graph,
                                                const PartitionSpec& spec) const override;

  void AppendBodyItems(std::vector<Doc>* body_items) const override;

 public:
  static constexpr const char* _type_key = "relay.collage.HostPartitionRule";
  TVM_DECLARE_FINAL_OBJECT_INFO(HostPartitionRuleNode, PartitionRuleNode);
};

class HostPartitionRule : public PartitionRule {
 public:
  explicit HostPartitionRule(String rule_name);

  TVM_DEFINE_OBJECT_REF_METHODS(HostPartitionRule, PartitionRule, HostPartitionRuleNode);
};

}  // namespace collage
}  // namespace relay
}  // namespace tvm

#endif  // TVM_RELAY_COLLAGE_PARTITION_RULE_H_

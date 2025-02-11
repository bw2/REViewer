//
// REViewer
// Copyright 2020 Illumina, Inc.
//
// Author: Egor Dolzhenko <edolzhenko@illumina.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "app/GenotypePaths.hh"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

using boost::optional;
using graphtools::Graph;
using graphtools::NodeId;
using graphtools::Path;
using std::map;
using std::pair;
using std::runtime_error;
using std::stoi;
using std::string;
using std::vector;

static vector<int> extractRepeatLengths(const string& vcfPath, const string& repeatId)
{
    std::ifstream vcfFile(vcfPath);
    if (vcfFile.is_open())
    {
        const string query = "VARID=" + repeatId + ";";
        string line;
        while (getline(vcfFile, line))
        {
            if (boost::find_first(line, query))
            {
                vector<string> pieces;
                boost::split(pieces, line, boost::is_any_of("\t"));
                const string sampleFields = pieces[pieces.size() - 1];
                boost::split(pieces, sampleFields, boost::is_any_of(":"));
                const string genotypeEncoding = pieces[2];

                if (genotypeEncoding == "./.")
                {
                    throw runtime_error("Cannot create a plot because the genotype of " + repeatId + " is missing");
                }

                boost::split(pieces, genotypeEncoding, boost::is_any_of("/"));
                vector<int> sizes;
                for (const auto& sizeEncoding : pieces)
                {
                    sizes.push_back(stoi(sizeEncoding));
                }
                return sizes;
            }
        }
        vcfFile.close();
    }
    else
    {
        throw std::runtime_error("Unable to open file " + vcfPath);
    }

    throw std::runtime_error("No VCF record for " + repeatId);
}

static vector<int> capLengths(int upperBound, const vector<int>& lengths)
{
    vector<int> cappedLength;
    cappedLength.reserve(lengths.size());
    for (int length : lengths)
    {
        cappedLength.push_back(length <= upperBound ? length : upperBound);
    }

    return cappedLength;
}

using NodeRange = pair<NodeId, NodeId>;
using Nodes = std::vector<graphtools::NodeId>;
using NodeVector = std::vector<graphtools::NodeId>;
using NodeVectors = std::vector<NodeVector>;

/// Determine sequences of nodes corresponding to each allele of the given varian
/// \param meanFragLen: Mean fragment length
/// \param vcfPath: Path to VCF file generated by ExpansionHunter
/// \param locusSpec: Description of the target locus
/// \return Sequences of nodes for each allele indexed by the range of nodes corresponding to the entire variant
///
/// Assumption: Locus contains only STRs
/// Detail: STR lengths are capped by fragment length (see implementation)
/// Example:
///  An STR corresponding to RE (CAG)* with genotype 3/4 corresponds to the
///  output {{1, 1}: {{1, 1, 1}, {1, 1, 1, 1}}
map<NodeRange, NodeVectors>
getGenotypeNodesByNodeRange(int meanFragLen, const string& vcfPath, const LocusSpecification& locusSpec)
{
    map<NodeRange, NodeVectors> genotypeNodesByNodeRange;
    for (const auto& variantSpec : locusSpec.variantSpecs())
    {
        if (variantSpec.classification().type == VariantType::kSmallVariant)
        {
            throw std::logic_error("REViewer does not accept locus definitions containing small variants (e.g. '(A|T)').");
        }
        assert(variantSpec.classification().type == VariantType::kRepeat);
        assert(variantSpec.nodes().size() == 1);
        NodeId repeatNode = variantSpec.nodes().front();
        NodeVectors genotypeNodes;

        auto repeatLens = extractRepeatLengths(vcfPath, variantSpec.id());
        repeatLens = capLengths(meanFragLen, repeatLens);

        genotypeNodes.reserve(repeatLens.size());
        for (int repeatLen : repeatLens)
        {
            genotypeNodes.emplace_back(repeatLen, repeatNode);
        }

        NodeId nodeRangeFrom = std::numeric_limits<NodeId>::max();
        NodeId nodeRangeTo = std::numeric_limits<NodeId>::lowest();

        for (NodeId node : variantSpec.nodes())
        {
            if (node != -1)
            {
                nodeRangeFrom = std::min(nodeRangeFrom, node);
                nodeRangeTo = std::max(nodeRangeTo, node);
            }
        }

        assert(genotypeNodes.size() <= 2);
        genotypeNodesByNodeRange.emplace(std::make_pair(nodeRangeFrom, nodeRangeTo), genotypeNodes);
    }

    return genotypeNodesByNodeRange;
}

static optional<pair<NodeVectors, NodeId>>
getVariantGenotypeNodes(const map<NodeRange, NodeVectors>& nodeRangeToPaths, NodeId node)
{
    for (const auto& nodeRangeAndPaths : nodeRangeToPaths)
    {
        const auto& nodeRange = nodeRangeAndPaths.first;
        const auto& paths = nodeRangeAndPaths.second;
        if (nodeRange.first <= node && node <= nodeRange.second)
        {
            assert(paths.size() <= 2);
            return std::make_pair(paths, nodeRange.second);
        }
    }

    return boost::none;
}

static vector<NodeVectors> extendDiplotype(const vector<NodeVectors>& genotypes, const NodeVectors& genotypeExtension)
{
    vector<NodeVectors> extendedGenotype;
    for (auto& genotype : genotypes)
    {
        assert(genotype.size() == genotypeExtension.size());
        if (genotype.size() == 1)
        {
            const Nodes& haplotypeExtension = genotypeExtension.front();
            Nodes extendedHaplotype = genotype.front();
            extendedHaplotype.insert(extendedHaplotype.end(), haplotypeExtension.begin(), haplotypeExtension.end());
            extendedGenotype.push_back({ extendedHaplotype });
        }
        else
        {
            assert(genotype.size() == 2);
            Nodes hap1Ext1 = genotype.front();
            hap1Ext1.insert(hap1Ext1.end(), genotypeExtension.front().begin(), genotypeExtension.front().end());

            Nodes hap2Ext2 = genotype.back();
            hap2Ext2.insert(hap2Ext2.end(), genotypeExtension.back().begin(), genotypeExtension.back().end());

            extendedGenotype.push_back({ hap1Ext1, hap2Ext2 });

            Nodes hap1Ext2 = genotype.front();
            hap1Ext2.insert(hap1Ext2.end(), genotypeExtension.back().begin(), genotypeExtension.back().end());

            Nodes hap2Ext1 = genotype.back();
            hap2Ext1.insert(hap2Ext1.end(), genotypeExtension.front().begin(), genotypeExtension.front().end());

            extendedGenotype.push_back({ hap1Ext2, hap2Ext1 });
        }
    }

    return extendedGenotype;
}

vector<Diplotype> getCandidateDiplotypes(int meanFragLen, const string& vcfPath, const LocusSpecification& locusSpec)
{
    auto genotypeNodesByNodeRange = getGenotypeNodesByNodeRange(meanFragLen, vcfPath, locusSpec);

    // Assume that all variants have the same number of alleles
    const auto numAlleles = genotypeNodesByNodeRange.begin()->second.size();

    vector<NodeVectors> nodesByDiplotype = { NodeVectors(numAlleles, { 0 }) };

    NodeId node = 1;
    while (node != locusSpec.regionGraph().numNodes())
    {
        auto variantPathNodesAndLastNode = getVariantGenotypeNodes(genotypeNodesByNodeRange, node);
        if (variantPathNodesAndLastNode)
        {
            nodesByDiplotype = extendDiplotype(nodesByDiplotype, variantPathNodesAndLastNode->first);
            node = variantPathNodesAndLastNode->second;
        }
        else
        {
            for (auto& genotypeNodes : nodesByDiplotype)
            {
                for (auto& haplotypeNodes : genotypeNodes)
                {
                    haplotypeNodes.push_back(node);
                }
            }
        }

        ++node;
    }

    vector<Diplotype> diplotypes;
    const NodeId rightFlankNode = locusSpec.regionGraph().numNodes() - 1;
    const int rightFlankLength = locusSpec.regionGraph().nodeSeq(rightFlankNode).length();
    for (const auto& diplotypeNodes : nodesByDiplotype)
    {
        Diplotype diplotype;
        for (const auto& haplotypeNodes : diplotypeNodes)
        {
            diplotype.emplace_back(&locusSpec.regionGraph(), 0, haplotypeNodes, rightFlankLength);
        }

        // The code so far considers diplotypes that differ by the order of constituent haplotypes to be distinct.
        // To overcome this issue, we enforce a consistent haplotype order.
        if (diplotype.front() < diplotype.back())
        {
            std::iter_swap(diplotype.begin(), diplotype.end() - 1);
        }

        diplotypes.push_back(diplotype);
    }

    std::sort(diplotypes.begin(), diplotypes.end());
    diplotypes.erase(std::unique(diplotypes.begin(), diplotypes.end()), diplotypes.end());

    return diplotypes;
}

static string summarizePath(const graphtools::Path& path)
{
    const auto& graph = *path.graphRawPtr();
    string summary;
    std::set<graphtools::NodeId> observedNodes;
    for (const auto nodeId : path.nodeIds())
    {
        if (observedNodes.find(nodeId) != observedNodes.end())
        {
            continue;
        }

        const bool isLoopNode = graph.hasEdge(nodeId, nodeId);

        if (nodeId == 0)
        {
            summary += "(LF)";
        }
        else if (nodeId + 1 == graph.numNodes())
        {
            summary += "(RF)";
        }
        else
        {
            assert(graph.numNodes() != 0);
            const string& nodeSeq = graph.nodeSeq(nodeId);
            summary += "(" + nodeSeq + ")";

            if (isLoopNode)
            {
                int numMotifs = std::count(path.nodeIds().begin(), path.nodeIds().end(), nodeId);
                summary += "{" + std::to_string(numMotifs) + "}";
            }
        }

        observedNodes.emplace(nodeId);
    }

    return summary;
}

std::ostream& operator<<(std::ostream& out, const Diplotype& diplotype)
{
    out << summarizePath(diplotype.front());
    if (diplotype.size() == 2)
    {
        out << "/" << summarizePath(diplotype.back());
    }
    return out;
}

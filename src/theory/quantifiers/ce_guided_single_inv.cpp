/*********************                                                        */
/*! \file ce_guided_single_inv.cpp
 ** \verbatim
 ** Original author: Andrew Reynolds
 ** Major contributors: none
 ** Minor contributors (to current version): none
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief utility for processing single invocation synthesis conjectures
 **
 **/

#include "theory/quantifiers/ce_guided_single_inv.h"
#include "theory/quantifiers/ce_guided_instantiation.h"
#include "theory/theory_engine.h"
#include "theory/quantifiers/options.h"
#include "theory/quantifiers/term_database.h"
#include "theory/quantifiers/first_order_model.h"
#include "theory/datatypes/datatypes_rewriter.h"
#include "util/datatype.h"
#include "theory/quantifiers/quant_util.h"

using namespace CVC4;
using namespace CVC4::kind;
using namespace CVC4::theory;
using namespace CVC4::theory::quantifiers;
using namespace std;

namespace CVC4 {

CegConjectureSingleInv::CegConjectureSingleInv( Node q, CegConjecture * p ) : d_parent( p ), d_quant( q ){

}

Node CegConjectureSingleInv::getSingleInvLemma( Node guard ) {
  if( !d_single_inv.isNull() ) {
    Assert( d_single_inv.getKind()==FORALL );
    d_single_inv_var.clear();
    d_single_inv_sk.clear();
    for( unsigned i=0; i<d_single_inv[0].getNumChildren(); i++ ){
      std::stringstream ss;
      ss << "k_" << d_single_inv[0][i];
      Node k = NodeManager::currentNM()->mkSkolem( ss.str(), d_single_inv[0][i].getType(), "single invocation function skolem" );
      d_single_inv_var.push_back( d_single_inv[0][i] );
      d_single_inv_sk.push_back( k );
      d_single_inv_sk_index[k] = i;
    }
    Node inst = d_single_inv[1].substitute( d_single_inv_var.begin(), d_single_inv_var.end(), d_single_inv_sk.begin(), d_single_inv_sk.end() );
    Trace("cegqi-si") << "Single invocation initial lemma : " << inst << std::endl;
    return NodeManager::currentNM()->mkNode( OR, guard.negate(), inst.negate() );
  }else{
    return Node::null();
  }
}

void CegConjectureSingleInv::initialize() {
  Node q = d_quant;
  Trace("cegqi-si") << "Initialize cegqi-si for " << q << std::endl;
  // conj -> conj*
  std::map< Node, std::vector< Node > > children;
  // conj X prog -> inv*
  std::map< Node, std::map< Node, std::vector< Node > > > prog_invoke;
  std::vector< Node > progs;
  std::map< Node, std::map< Node, bool > > contains;
  for( unsigned i=0; i<q[0].getNumChildren(); i++ ){
    progs.push_back( q[0][i] );
  }
  //collect information about conjunctions
  if( analyzeSygusConjunct( Node::null(), q[1], children, prog_invoke, progs, contains, true ) ){
    //try to phrase as single invocation property
    bool singleInvocation = true;
    Trace("cegqi-si") << "...success." << std::endl;
    std::map< Node, std::vector< Node > > invocations;
    std::map< Node, std::map< int, std::vector< Node > > > single_invoke_args;
    std::map< Node, std::map< int, std::map< Node, std::vector< Node > > > > single_invoke_args_from;
    for( std::map< Node, std::vector< Node > >::iterator it = children.begin(); it != children.end(); ++it ){
      for( unsigned j=0; j<it->second.size(); j++ ){
        Node conj = it->second[j];
        Trace("cegqi-si-debug") << "Process child " << conj << " from " << it->first << std::endl;
        std::map< Node, std::map< Node, std::vector< Node > > >::iterator itp = prog_invoke.find( conj );
        if( itp!=prog_invoke.end() ){
          for( std::map< Node, std::vector< Node > >::iterator it2 = itp->second.begin(); it2 != itp->second.end(); ++it2 ){
            if( it2->second.size()>1 ){
              singleInvocation = false;
              break;
            }else if( it2->second.size()==1 ){
              Node prog = it2->first;
              Node inv = it2->second[0];
              Assert( inv[0]==prog );
              invocations[prog].push_back( inv );
              for( unsigned k=1; k<inv.getNumChildren(); k++ ){
                Node arg = inv[k];
                Trace("cegqi-si-debug") << "process : " << arg << " occurs in position " << k-1 << " in invocation " << inv << " of " << prog << " in " << conj << std::endl;
                single_invoke_args_from[prog][k-1][arg].push_back( conj );
                if( std::find( single_invoke_args[prog][k-1].begin(), single_invoke_args[prog][k-1].end(), arg )==single_invoke_args[prog][k-1].end() ){
                  single_invoke_args[prog][k-1].push_back( arg );
                }
              }
            }
          }
        }
      }
    }
    if( singleInvocation ){
      Trace("cegqi-si") << "Property is single invocation with : " << std::endl;
      std::vector< Node > pbvs;
      std::vector< Node > new_vars;
      std::map< Node, std::vector< Node > > new_assumptions;
      for( std::map< Node, std::vector< Node > >::iterator it = invocations.begin(); it != invocations.end(); ++it ){
        Assert( !it->second.empty() );
        Node prog = it->first;
        Node inv = it->second[0];
        std::vector< Node > invc;
        invc.push_back( inv.getOperator() );
        invc.push_back( prog );
        std::stringstream ss;
        ss << "F_" << prog;
        Node pv = NodeManager::currentNM()->mkBoundVar( ss.str(), inv.getType() );
        d_single_inv_map[prog] = pv;
        d_single_inv_map_to_prog[pv] = prog;
        pbvs.push_back( pv );
        Trace("cegqi-si-debug2") << "Make variable " << pv << " for " << prog << std::endl;
        for( unsigned k=1; k<inv.getNumChildren(); k++ ){
          Assert( !single_invoke_args[prog][k-1].empty() );
          if( single_invoke_args[prog][k-1].size()==1 ){
            invc.push_back( single_invoke_args[prog][k-1][0] );
          }else{
            //introduce fresh variable, assign all
            Node v = NodeManager::currentNM()->mkSkolem( "a", single_invoke_args[prog][k-1][0].getType(), "single invocation arg" );
            new_vars.push_back( v );
            invc.push_back( v );

            for( unsigned i=0; i<single_invoke_args[prog][k-1].size(); i++ ){
              Node arg = single_invoke_args[prog][k-1][i];
              Node asum = NodeManager::currentNM()->mkNode( arg.getType().isBoolean() ? IFF : EQUAL, v, arg ).negate();
              Trace("cegqi-si-debug") << "  ..." << arg << " occurs in ";
              Trace("cegqi-si-debug") << single_invoke_args_from[prog][k-1][arg].size() << " invocations at position " << (k-1) << " of " << prog << "." << std::endl;
              for( unsigned j=0; j<single_invoke_args_from[prog][k-1][arg].size(); j++ ){
                Node conj = single_invoke_args_from[prog][k-1][arg][j];
                Trace("cegqi-si-debug") << "  ..." << arg << " occurs in invocation " << inv << " of " << prog << " in " << conj << std::endl;
                Trace("cegqi-si-debug") << "  ...add assumption " << asum << " to " << conj << std::endl;
                if( std::find( new_assumptions[conj].begin(), new_assumptions[conj].end(), asum )==new_assumptions[conj].end() ){
                  new_assumptions[conj].push_back( asum );
                }
              }
            }
          }
        }
        Node sinv = NodeManager::currentNM()->mkNode( APPLY_UF, invc );
        Trace("cegqi-si") << "  " << prog << " -> " << sinv << std::endl;
        d_single_inv_app_map[prog] = sinv;
      }
      //construct the single invocation version of the property
      Trace("cegqi-si") << "Single invocation formula conjuncts are : " << std::endl;
      //std::vector< Node > si_conj;
      Assert( !pbvs.empty() );
      Node pbvl = NodeManager::currentNM()->mkNode( BOUND_VAR_LIST, pbvs );
      for( std::map< Node, std::vector< Node > >::iterator it = children.begin(); it != children.end(); ++it ){
        //should hold since we prevent miniscoping
        Assert( d_single_inv.isNull() );
        std::vector< Node > tmp;
        for( unsigned i=0; i<it->second.size(); i++ ){
          Node c = it->second[i];
          std::vector< Node > disj;
          //insert new assumptions
          disj.insert( disj.end(), new_assumptions[c].begin(), new_assumptions[c].end() );
          //get replaced version
          Node cr;
          std::map< Node, std::map< Node, std::vector< Node > > >::iterator itp = prog_invoke.find( c );
          if( itp!=prog_invoke.end() ){
            std::vector< Node > terms;
            std::vector< Node > subs;
            for( std::map< Node, std::vector< Node > >::iterator it2 = itp->second.begin(); it2 != itp->second.end(); ++it2 ){
              if( !it2->second.empty() ){
                Node prog = it2->first;
                Node inv = it2->second[0];
                Assert( it2->second.size()==1 );
                terms.push_back( inv );
                subs.push_back( d_single_inv_map[prog] );
                Trace("cegqi-si-debug2") << "subs : " << inv << " -> var for " << prog << " : " << d_single_inv_map[prog] << std::endl;
              }
            }
            cr = c.substitute( terms.begin(), terms.end(), subs.begin(), subs.end() );
          }else{
            cr = c;
          }
          if( cr.getKind()==OR ){
            for( unsigned j=0; j<cr.getNumChildren(); j++ ){
              disj.push_back( cr[j] );
            }
          }else{
            disj.push_back( cr );
          }
          Node curr = disj.size()==1 ? disj[0] : NodeManager::currentNM()->mkNode( OR, disj );
          Trace("cegqi-si") << "    " << curr;
          tmp.push_back( curr.negate() );
          if( !it->first.isNull() ){
            Trace("cegqi-si-debug") << " under " << it->first;
          }
          Trace("cegqi-si") << std::endl;
        }
        Assert( !tmp.empty() );
        Node bd = tmp.size()==1 ? tmp[0] : NodeManager::currentNM()->mkNode( OR, tmp );
        if( !it->first.isNull() ){
          // apply substitution for skolem variables
          std::vector< Node > vars;
          d_single_inv_arg_sk.clear();
          for( unsigned j=0; j<it->first.getNumChildren(); j++ ){
            std::stringstream ss;
            ss << "k_" << it->first[j];
            Node v = NodeManager::currentNM()->mkSkolem( ss.str(), it->first[j].getType(), "single invocation skolem" );
            vars.push_back( it->first[j] );
            d_single_inv_arg_sk.push_back( v );
          }
          bd = bd.substitute( vars.begin(), vars.end(), d_single_inv_arg_sk.begin(), d_single_inv_arg_sk.end() );

          Trace("cegqi-si-debug") << "    -> " << bd << std::endl;
        }
        d_single_inv = NodeManager::currentNM()->mkNode( FORALL, pbvl, bd );
        //equality resolution
        for( unsigned j=0; j<tmp.size(); j++ ){
          Node conj = tmp[j];
          std::map< Node, std::vector< Node > > case_vals;
          bool exh = processSingleInvLiteral( conj, false, case_vals );
          Trace("cegqi-si-er") << "Conjunct " << conj << " gives equality restrictions, exh = " << exh << " : " << std::endl;
          for( std::map< Node, std::vector< Node > >::iterator it = case_vals.begin(); it != case_vals.end(); ++it ){
            Trace("cegqi-si-er") << "  " << it->first << " -> ";
            for( unsigned k=0; k<it->second.size(); k++ ){
              Trace("cegqi-si-er") << it->second[k] << " ";
            }
            Trace("cegqi-si-er") << std::endl;
          }

        }
      }
      Trace("cegqi-si-debug") << "...formula is : " << d_single_inv << std::endl;
    }else{
      Trace("cegqi-si") << "Property is not single invocation." << std::endl;
      if( options::cegqiSingleInvAbort() ){
        Message() << "Property is not single invocation." << std::endl;
        exit( 0 );
      }
    }
  }
}

bool CegConjectureSingleInv::processSingleInvLiteral( Node lit, bool pol, std::map< Node, std::vector< Node > >& case_vals ) {
  if( lit.getKind()==NOT ){
    return processSingleInvLiteral( lit[0], !pol, case_vals );
  }else if( ( lit.getKind()==OR && pol ) || ( lit.getKind()==AND && !pol ) ){
    bool exh = true;
    for( unsigned k=0; k<lit.getNumChildren(); k++ ){
      bool curr = processSingleInvLiteral( lit[k], pol, case_vals );
      exh = exh && curr;
    }
    return exh;
  }else if( lit.getKind()==IFF || lit.getKind()==EQUAL ){
    if( pol ){
      for( unsigned r=0; r<2; r++ ){
        std::map< Node, Node >::iterator it = d_single_inv_map_to_prog.find( lit[r] );
        if( it!=d_single_inv_map_to_prog.end() ){
          if( r==1 || d_single_inv_map_to_prog.find( lit[1] )==d_single_inv_map_to_prog.end() ){
            case_vals[it->second].push_back( lit[ r==0 ? 1 : 0 ] );
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool CegConjectureSingleInv::analyzeSygusConjunct( Node p, Node n, std::map< Node, std::vector< Node > >& children,
                                                            std::map< Node, std::map< Node, std::vector< Node > > >& prog_invoke,
                                                            std::vector< Node >& progs, std::map< Node, std::map< Node, bool > >& contains, bool pol ) {
  if( ( pol && n.getKind()==OR ) || ( !pol && n.getKind()==AND ) ){
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      if( !analyzeSygusConjunct( p, n[i], children, prog_invoke, progs, contains, pol ) ){
        return false;
      }
    }
  }else if( pol && n.getKind()==NOT && n[0].getKind()==FORALL ){
    if( !p.isNull() ){
      //do not allow nested quantifiers
      return false;
    }
    analyzeSygusConjunct( n[0][0], n[0][1], children, prog_invoke, progs, contains, false );
  }else{
    if( pol ){
      n = n.negate();
    }
    Trace("cegqi-si") << "Sygus conjunct : " << n << std::endl;
    children[p].push_back( n );
    for( unsigned i=0; i<progs.size(); i++ ){
      prog_invoke[n][progs[i]].clear();
    }
    bool success = analyzeSygusTerm( n, prog_invoke[n], contains[n] );
    for( unsigned i=0; i<progs.size(); i++ ){
      std::map< Node, std::vector< Node > >::iterator it = prog_invoke[n].find( progs[i] );
      Trace("cegqi-si") << "  Program " << progs[i] << " is invoked " << it->second.size() << " times " << std::endl;
      for( unsigned j=0; j<it->second.size(); j++ ){
        Trace("cegqi-si") << "    " << it->second[j] << std::endl;
      }
    }
    return success;
  }
  return true;
}

bool CegConjectureSingleInv::analyzeSygusTerm( Node n, std::map< Node, std::vector< Node > >& prog_invoke, std::map< Node, bool >& contains ) {
  if( n.getNumChildren()>0 ){
    if( n.getKind()==FORALL ){
      //do not allow nested quantifiers
      return false;
    }
    //look at first argument in evaluator
    Node p = n[0];
    std::map< Node, std::vector< Node > >::iterator it = prog_invoke.find( p );
    if( it!=prog_invoke.end() ){
      if( std::find( it->second.begin(), it->second.end(), n )==it->second.end() ){
        it->second.push_back( n );
      }
    }
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      if( !analyzeSygusTerm( n[i], prog_invoke, contains ) ){
        return false;
      }
    }
  }else{
    //record this conjunct contains n
    contains[n] = true;
  }
  return true;
}


void CegConjectureSingleInv::check( QuantifiersEngine * qe, std::vector< Node >& lems ) {
  if( !d_single_inv.isNull() ) { 
    eq::EqualityEngine* ee = qe->getMasterEqualityEngine();
    Trace("cegqi-engine") << "  * single invocation: " << std::endl;
    std::vector< Node > subs;
    std::map< Node, int > subs_from_model;
    std::vector< Node > waiting_to_slv;
    for( unsigned i=0; i<d_single_inv_sk.size(); i++ ){
      Node v = d_single_inv_map_to_prog[d_single_inv[0][i]];
      Node pv = d_single_inv_sk[i];
      Trace("cegqi-engine") << "    * " << v;
      Trace("cegqi-engine") << " (" << pv << ")";
      Trace("cegqi-engine") << " -> "; 
      Node slv;
      if( ee->hasTerm( pv ) ){
        Node r = ee->getRepresentative( pv );
        eq::EqClassIterator eqc_i = eq::EqClassIterator( r, ee );
        bool isWaitingSlv = false;
        while( !eqc_i.isFinished() ){
          Node n = *eqc_i;
          if( n!=pv ){
            if( slv.isNull() || isWaitingSlv ){
              std::vector< Node > vars;
              collectProgVars( n, vars );
              if( slv.isNull() || vars.empty() ){
                // n cannot contain pv
                bool isLoop = std::find( vars.begin(), vars.end(), pv )!=vars.end();
                if( !isLoop ){
                  for( unsigned j=0; j<vars.size(); j++ ){
                    if( std::find( waiting_to_slv.begin(), waiting_to_slv.end(), vars[j] )!=waiting_to_slv.end() ){
                      isLoop = true;
                      break;
                    }
                  }
                  if( !isLoop ){
                    slv = n;
                    isWaitingSlv = !vars.empty();
                  }
                }
              }
            }
          }
          ++eqc_i;
        }
        if( isWaitingSlv ){
          Trace("cegqi-engine-debug") << "waiting:";
          waiting_to_slv.push_back( pv );
        }
      }else{
        Trace("cegqi-engine-debug") << "N/A:";
      }
      if( slv.isNull() ){
        //get model value
        Node mv = qe->getModel()->getValue( pv );
        subs.push_back( mv );
        subs_from_model[pv] = i;
        if( Trace.isOn("cegqi-engine") || Trace.isOn("cegqi-engine-debug") ){
          Trace("cegqi-engine") << "M:" << mv;
        }
      }else{
        subs.push_back( slv );
        Trace("cegqi-engine") << slv;
      }
      Trace("cegqi-engine") << std::endl;
    }
    for( int i=(int)(waiting_to_slv.size()-1); i>=0; --i ){
      int index = d_single_inv_sk_index[waiting_to_slv[i]];
      Trace("cegqi-engine-debug") << "Go back and solve " << d_single_inv_sk[index] << std::endl;
      Trace("cegqi-engine-debug") << "Substitute " << subs[index] << " to ";
      subs[index] = applyProgVarSubstitution( subs[index], subs_from_model, subs );
      Trace("cegqi-engine-debug") << subs[index] << std::endl;
    }
    //try to improve substitutions : look for terms that contain terms in question
    if( !subs_from_model.empty() ){
      eq::EqClassesIterator eqcs_i = eq::EqClassesIterator( ee );
      while( !eqcs_i.isFinished() ){
        Node r = *eqcs_i;
        int res = -1;
        Node slv_n;
        Node const_n;
        eq::EqClassIterator eqc_i = eq::EqClassIterator( r, ee );
        while( !eqc_i.isFinished() ){
          Node n = *eqc_i;
          if( slv_n.isNull() || const_n.isNull() ){
            std::vector< Node > vars;
            int curr_res = classifyTerm( n, subs_from_model );
            Trace("cegqi-si-debug2") << "Term : " << n << " classify : " << curr_res << std::endl;
            if( curr_res!=-2 ){
              if( curr_res!=-1 && slv_n.isNull() ){
                res = curr_res;
                slv_n = n;
              }else if( const_n.isNull() ){
                const_n = n;
              }
            }
          }
          ++eqc_i;
        }
        if( !slv_n.isNull() && !const_n.isNull() ){
          if( slv_n.getType().isInteger() || slv_n.getType().isReal() ){
            Trace("cegqi-si-debug") << "Can possibly set " << d_single_inv_sk[res] << " based on " << slv_n << " = " << const_n << std::endl;
            subs_from_model.erase( d_single_inv_sk[res] );
            Node prev_subs_res = subs[res];
            subs[res] = d_single_inv_sk[res];
            Node eq = slv_n.eqNode( const_n );
            bool success = false;
            std::map< Node, Node > msum;
            if( QuantArith::getMonomialSumLit( eq, msum ) ){
              Trace("cegqi-si-debug") << "As monomial sum : " << std::endl;
              QuantArith::debugPrintMonomialSum( msum, "cegqi-si");
              Node veq;
              if( QuantArith::isolate( d_single_inv_sk[res], msum, veq, EQUAL ) ){
                Trace("cegqi-si-debug") << "Isolated for " << d_single_inv_sk[res] << " : " << veq << std::endl;
                Node sol;
                for( unsigned r=0; r<2; r++ ){
                  if( veq[r]==d_single_inv_sk[res] ){
                    sol = veq[ r==0 ? 1 : 0 ];
                  }
                }
                if( !sol.isNull() ){
                  sol = applyProgVarSubstitution( sol, subs_from_model, subs );
                  Trace("cegqi-si-debug") << "Substituted solution is : " << sol << std::endl;
                  subs[res] = sol;
                  Trace("cegqi-engine") << "  ...by arithmetic, " << d_single_inv_sk[res] << " -> " << sol << std::endl;
                  success = true;
                }
              }
            }
            if( !success ){
              subs[res] = prev_subs_res;
            }
          }
        }
        ++eqcs_i;
      }
    }
    Node lem = d_single_inv[1].substitute( d_single_inv_var.begin(), d_single_inv_var.end(), subs.begin(), subs.end() );
    lem = Rewriter::rewrite( lem );
    Trace("cegqi-si") << "Single invocation lemma : " << lem << std::endl;
    if( std::find( d_lemmas_produced.begin(), d_lemmas_produced.end(), lem )==d_lemmas_produced.end() ){
      lems.push_back( lem );
      d_lemmas_produced.push_back( lem );
      d_inst.push_back( std::vector< Node >() );
      d_inst.back().insert( d_inst.back().end(), subs.begin(), subs.end() );
    }
  }
}

Node CegConjectureSingleInv::applyProgVarSubstitution( Node n, std::map< Node, int >& subs_from_model, std::vector< Node >& subs ) {
  std::vector< Node > vars;
  collectProgVars( n, vars );
  if( !vars.empty() ){
    std::vector< Node > ssubs;
    //get substitution
    for( unsigned i=0; i<vars.size(); i++ ){
      Assert( d_single_inv_sk_index.find( vars[i] )!=d_single_inv_sk_index.end() );
      int index = d_single_inv_sk_index[vars[i]];
      ssubs.push_back( subs[index] );
      //it is now constrained
      if( subs_from_model.find( vars[i] )!=subs_from_model.end() ){
        subs_from_model.erase( vars[i] );
      }
    }
    return n.substitute( vars.begin(), vars.end(), ssubs.begin(), ssubs.end() );
  }else{
    return n;
  }
}

int CegConjectureSingleInv::classifyTerm( Node n, std::map< Node, int >& subs_from_model ) {
  if( n.getKind()==SKOLEM ){
    std::map< Node, int >::iterator it = subs_from_model.find( n );
    if( it!=subs_from_model.end() ){
      return it->second;
    }else{
      // if it is symbolic argument, we are also fine
      if( std::find( d_single_inv_arg_sk.begin(), d_single_inv_arg_sk.end(), n )!=d_single_inv_arg_sk.end() ){
        return -1;
      }else{
        //if it is another program, we are also fine
        if( std::find( d_single_inv_sk.begin(), d_single_inv_sk.end(), n )!=d_single_inv_sk.end() ){
          return -1;
        }else{
          return -2;
        }
      }
    }
  }else{
    int curr_res = -1;
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      int res = classifyTerm( n[i], subs_from_model );
      if( res==-2 ){
        return res;
      }else if( res!=-1 ){
        curr_res = res;
      }
    }
    return curr_res;
  }
}

void CegConjectureSingleInv::collectProgVars( Node n, std::vector< Node >& vars ) {
  if( n.getKind()==SKOLEM ){
    if( std::find( d_single_inv_sk.begin(), d_single_inv_sk.end(), n )!=d_single_inv_sk.end() ){
      if( std::find( vars.begin(), vars.end(), n )==vars.end() ){
        vars.push_back( n );
      }
    }
  }else{
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      collectProgVars( n[i], vars );
    }
  }
}

Node CegConjectureSingleInv::constructSolution( unsigned i, unsigned index ) {
  Assert( index<d_inst.size() );
  Assert( i<d_inst[index].size() );
  if( index==d_inst.size()-1 ){
    return d_inst[index][i];
  }else{
    Node cond = d_lemmas_produced[index];
    Node ite1 = d_inst[index][i];
    Node ite2 = constructSolution( i, index+1 );
    return NodeManager::currentNM()->mkNode( ITE, cond, ite1, ite2 );
  }
}

Node CegConjectureSingleInv::getSolution( unsigned i, Node varList ){
  Assert( !d_lemmas_produced.empty() );
  Node s = constructSolution( i, 0 );
  //TODO : not in grammar
  std::vector< Node > vars;
  for( unsigned i=0; i<varList.getNumChildren(); i++ ){
    vars.push_back( varList[i] );
  }
  Assert( vars.size()==d_single_inv_arg_sk.size() );
  s = s.substitute( d_single_inv_arg_sk.begin(), d_single_inv_arg_sk.end(), vars.begin(), vars.end() );
  return s;  
}

}
/**
 * parser.cpp
 *
 *  Created on: Jan 31, 2017
 *      Author: asaparov
 */

#include "datalog.h"
#include "datalog_hdp.h"
#include "parser_data.h"

#include <atomic>
#include <thread>
#include <grammar/hdp_grammar.h>
#include <grammar/hdp_grammar_io.h>
#include <grammar/parser.h>

using namespace core;

typedef sequence_distribution<token_distribution<double>> terminal_prior_type;
typedef hdp_grammar<rule_list_prior<terminal_prior<terminal_prior_type>, datalog_expression_root>, datalog_expression_root> hdp_grammar_type;

template<typename Stream>
bool read(unsigned int& token_id, Stream& stream, hash_map<string, unsigned int>& token_map) {
	string token;
	if (!read(token, stream)) return false;
	if (token == "<empty>") {
		token_id = DATALOG_LABEL_EMPTY;
		return true;
	} else {
		return get_token(token, token_id, token_map);
	}
}

template<typename Stream>
bool write(unsigned int token_id, Stream& stream, const string** token_map) {
	if (token_id == DATALOG_LABEL_EMPTY) {
		return write(string("<empty>"), stream);
	} else if (token_map[token_id] == NULL) {
		fprintf(stderr, "write ERROR: Encountered invalid token.\n");
		return false;
	}
	return write(*token_map[token_id], stream);
}

template<typename Stream>
inline bool print_special_string(unsigned int item, Stream& out) {
	if (item == DATALOG_LABEL_EMPTY)
		return print("[null]", out);
	else if (item == DATALOG_LABEL_WILDCARD)
		return print('*', out);
	else return print("<new token>", out);
}

double edge_hdp_alpha[] = { 1000.0, 1.0, 10.0 };
double constant_hdp_alpha[] = { 0.01, 0.1 };
datalog_prior prior(32, 5, 128, edge_hdp_alpha, constant_hdp_alpha);
datalog_ontology ontology;
morphology morph;
bool enable_morphology = true;
std::mutex console_lock;

thread_local bool debug_type_check = true;
thread_local bool disable_prior = false;

template<parse_mode Mode, bool Complete = false>
inline double log_probability(const datalog_expression_root& exp) {
	if (Mode == MODE_SAMPLE || disable_prior)
		return 0.0;
	if (!valid_variable_scope(exp.root) || (debug_type_check && !type_check<Complete>(ontology, exp)))
		return -std::numeric_limits<double>::infinity();
	//if (!Complete) return 0.0;
	return prior.log_probability<Complete>(exp);
}

inline const fixed_array<token>& morphology_parse(unsigned int word) {
#if !defined(NDEBUG)
	if (!enable_morphology)
		fprintf(stderr, "morphology_parse WARNING: Morphology model is disabled by command-line flag.\n");
#endif
	return morph.parse(word);
}

inline const fixed_array<unsigned int>& morphology_inflect(const token& tok) {
#if !defined(NDEBUG)
	if (!enable_morphology)
		fprintf(stderr, "morphology_inflect WARNING: Morphology model is disabled by command-line flag.\n");
#endif
	return morph.inflect(tok);
}

inline bool morphology_is_auxiliary_verb(unsigned int word) {
#if !defined(NDEBUG)
	if (!enable_morphology)
		fprintf(stderr, "morphology_is_auxiliary_verb WARNING: Morphology model is disabled by command-line flag.\n");
#endif
	return morph.is_auxiliary_verb(word);
}

inline bool morphology_is_auxiliary_root(unsigned int root) {
#if !defined(NDEBUG)
	if (!enable_morphology)
		fprintf(stderr, "morphology_is_auxiliary_root WARNING: Morphology model is disabled by command-line flag.\n");
#endif
	return morph.is_auxiliary_root(root);
}

template<typename Stream>
unsigned int read_line(array<char>& line, Stream& input)
{
	unsigned int bytes_read = 0;
	while (true) {
		int width;
		wint_t next = fgetwc(input);
		if (!line.ensure_capacity(line.length + MB_CUR_MAX)) {
			fprintf(stderr, "read_line ERROR: Out of memory.\n");
			return 0;
		}
		switch (next) {
		case WEOF:
			return bytes_read;

		case '\n':
			return bytes_read + 1;

		default:
#if defined(_WIN32)
			wctomb_s(&width, line.data + line.length, (line.capacity - line.length) * sizeof(char), next);
#else
			width = wctomb(line.data + line.length, next);
#endif
			if (width == -1)
				return 0;
			line.length += width;
			bytes_read += width;
		}
	}

	return bytes_read;
}

template<typename Stream>
void print_nonterminal_hdps(hdp_grammar_type& G, Stream& out, string_map_scribe& terminal_printer, string_map_scribe& nonterminal_printer) {
	auto printers = make_pair<string_map_scribe&, string_map_scribe&>(terminal_printer, nonterminal_printer);
	for (unsigned int i = 0; i < G.nonterminals.length; i++) {
		if (G.nonterminals[i].rule_distribution.observations.sum == 0) continue;
		print(G.nonterminals[i].rule_distribution.type, out); print(' ', out);
		print(G.nonterminals[i].name, out); fprintf(out, " (%u) HDP: ", G.nonterminals[i].id);
		print(G.nonterminals[i].rule_distribution.sampler, out, terminal_printer, printers); print('\n', out);
		print(G.nonterminals[i].rule_distribution.h.alpha, G.nonterminals[i].rule_distribution.feature_count + 1, out); print('\n', out);
	}
}

inline bool get_ngrams(const sequence& sentence,
		hash_map<sequence, unsigned int>& ngrams, unsigned int n)
{
	for (unsigned int i = n - 1; i < sentence.length; i++) {
		if (!ngrams.check_size()) return false;

		bool contains; unsigned int bucket;
		sequence ngram(sentence.tokens + i + 1 - n, n);
		unsigned int& count = ngrams.get(ngram, contains, bucket);
		if (!contains) {
			ngrams.table.keys[bucket] = ngram;
			ngrams.table.size++;
			count = 1;
		} else {
			count++;
		}
	}
	return true;
}

inline bool modified_precision(
		const sequence& reference, const sequence& hypothesis,
		unsigned int& numerator, unsigned int& denominator,
		unsigned int n)
{
	hash_map<sequence, unsigned int> hypothesis_ngrams(32);
	hash_map<sequence, unsigned int> reference_ngrams(32);
	if (!get_ngrams(hypothesis, hypothesis_ngrams, n)
	 || !get_ngrams(reference, reference_ngrams, n)) {
		for (auto entry : hypothesis_ngrams) free(entry.key);
		for (auto entry : reference_ngrams) free(entry.key);
	}

	hash_map<sequence, unsigned int> clipped_counts(32);
	for (auto entry : hypothesis_ngrams) {
		bool contains;
		unsigned int reference_count = reference_ngrams.get(entry.key, contains);
		if (!contains) clipped_counts.put(entry.key, 0);
		else clipped_counts.put(entry.key, min(entry.value, reference_count));
	}
	for (auto entry : reference_ngrams) free(entry.key);

	unsigned int clipped_count_sum = 0, hypothesis_ngram_sum = 0;
	for (auto entry : clipped_counts)
		clipped_count_sum += entry.value;
	for (auto entry : hypothesis_ngrams)
		hypothesis_ngram_sum += entry.value;

	numerator += clipped_count_sum;
	denominator += max(1u, hypothesis_ngram_sum);
	return true;
}

double bleu(const sequence& reference, const sequence& hypothesis) {
	constexpr double weights[] = {0.25, 0.25, 0.25, 0.25};

	unsigned int numerators[array_length(weights)];
	unsigned int denominators[array_length(weights)];
	for (unsigned int i = 0; i < array_length(weights); i++) {
		numerators[i] = 0; denominators[i] = 0;
	}

	/* iterate over each hypothesis and reference list */
	/* we only have 1 of each, so this is easy */
	for (unsigned int i = 0; i < array_length(weights); i++)
		modified_precision(reference, hypothesis, numerators[i], denominators[i], i + 1);

	if (numerators[0] == 0) return 0.0;

	double score = 0.0;
	for (unsigned int i = 0; i < array_length(weights); i++)
		score += weights[i] * log((double) (numerators[i] + 1) / (denominators[i] + 1));

	/* compute the brevity penalty */
	double penalty;
	if (hypothesis.length > reference.length) {
		penalty = 1.0;
	} else if (hypothesis.length == 0) {
		penalty = 0.0;
	} else {
		penalty = exp(1.0 - (double) reference.length / hypothesis.length);
	}

	return penalty * exp(score);
}

void parse(const hdp_grammar_type& G_src,
		const sequence* sentences,
		unsigned int sentence_count,
		const datalog_expression_root* const* logical_forms,
		const hash_set<unsigned int>& known_tokens,
		const string_map_scribe& terminal_printer,
		const string_map_scribe& nonterminal_printer, FILE* out,
		std::atomic_uint& unanswered,
		std::atomic_uint& incorrect,
		std::atomic_uint& counter,
		unsigned int time_limit)
{
	char prefix[16];
	hdp_grammar_type& G = *((hdp_grammar_type*) alloca(sizeof(hdp_grammar_type)));
	copy(G_src, G);
	while (true)
	{
		unsigned int id = counter++;
		if (id >= sentence_count)
			break;
//id = 83;
//if (counter > 1) break;

		bool skip = false;
		console_lock.lock();
		snprintf(prefix, array_length(prefix), "(%u) ", id);
		fprintf(out, "%sParsing sentence %u...\n", prefix, id); fflush(out);
		for (unsigned int j = 0; j < sentences[id].length; j++) {
			int value;
			unsigned int token = sentences[id].tokens[j];
			if (!known_tokens.contains(token) && !parse_int(*terminal_printer.map[token], value)) {
				fprintf(out, "%sThe token '", prefix); print(*terminal_printer.map[token], out); print("' is unrecognized.\n", out);
				skip = true;
			}
		}
		if (skip) {
			print('\n', out);
			console_lock.unlock();
			unanswered++; continue;
		}
		console_lock.unlock();
		parser_prefix = prefix;

		datalog_expression_root logical_form;
		logical_form.index = NUMBER_ALL;
		logical_form.concord = NUMBER_NONE;
		logical_form.inf = INFLECTION_NONE;
		syntax_node<datalog_expression_root>& parsed_syntax =
			*((syntax_node<datalog_expression_root>*) alloca(sizeof(syntax_node<datalog_expression_root>)));
		auto sentence = tokenized_sentence<datalog_expression_root>(sentences[id]);

		double true_log_likelihood = 1.0, true_log_prior = 1.0;
		datalog_expression_root true_logical_form = *logical_forms[id];
minimum_priority = 0.0;
		if (parse<false>(parsed_syntax, true_logical_form, G, sentence, terminal_printer.map, time_limit)) {
//debug2 = true;
//			print(parsed_syntax, out, nonterminal_printer, terminal_printer); print("\n", out);
			true_log_likelihood = log_probability(G, parsed_syntax, true_logical_form);
			true_log_prior = log_probability<MODE_PARSE, true>(true_logical_form);
//debug2 = false;
			datalog_expression_root logical_form_set;
			logical_form_set.index = NUMBER_ALL;
			logical_form_set.concord = NUMBER_NONE;
			logical_form_set.inf = INFLECTION_NONE;
//			fprintf(out, "%sParse log probability: %lf (prior: %lf)\n", prefix, true_log_likelihood, true_log_prior);
			is_parseable(parsed_syntax, true_logical_form, G, logical_form_set, nonterminal_printer, terminal_printer, terminal_printer.map);
			free(parsed_syntax);
			free(logical_form_set);
minimum_priority = exp(true_log_likelihood + true_log_prior - 1.0e-12);
		} else {
			fprintf(out, "%sWARNING: Unable to parse sentence %u with the true logical form.\n", prefix, id);
minimum_priority = 0.0;
		}
//debug_flag = true;
		if (parse<false>(parsed_syntax, logical_form, G, sentence, terminal_printer.map, time_limit)) {
			console_lock.lock();
			if (!equivalent(logical_form, *logical_forms[id])) {
				fprintf(out, "%sTrue logical form:      ", prefix); print(*logical_forms[id], out, terminal_printer); print('\n', out);
				fprintf(out, "%sPredicted logical form: ", prefix); print(logical_form, out, terminal_printer); print('\n', out);
//debug2 = true;
				print(parsed_syntax, out, nonterminal_printer, terminal_printer); print("\n", out);
				double predicted_log_likelihood = log_probability(G, parsed_syntax, logical_form);
				double predicted_log_prior = log_probability<MODE_PARSE, true>(logical_form);
//debug2 = false;
				fprintf(out, "%sParse log probability: %lf (prior: %lf)\n",
						prefix, predicted_log_likelihood, predicted_log_prior);
				if (true_log_likelihood != 1.0 && !std::isinf(predicted_log_likelihood)
				 && true_log_likelihood + true_log_prior > predicted_log_likelihood + predicted_log_prior)
					fprintf(out, "%sWARNING: The predicted derivation has lower probability than the true derivation.\n", prefix);
				incorrect++;
			}
			free(logical_form); free(parsed_syntax);
		} else {
			console_lock.lock();
			fprintf(out, "%sParser did not output a logical form.\n", prefix);
			unanswered++;
		}
		print('\n', out); fflush(out);
		console_lock.unlock();
	}
	free(G);
	return;
}

bool parse(hash_map<string, unsigned int>& names,
		const char* train_filepath, const char* extra_filepath,
		const char* kb_filepath, const char* test_filepath,
		const char* input_filepath, const char* ontology_filepath,
		unsigned int time_limit, unsigned int thread_count,
		bool test_parseability)
{
	disable_prior = false;
	array<datalog_expression_root*> train_data(1024), extra_data(128), kb_data(128), test_data(512);
	sequence* train_sentences;
	sequence* extra_sentences;
	sequence* test_sentences;
	datalog_expression_root** train_logical_forms;
	datalog_expression_root** extra_logical_forms;
	datalog_expression_root** kb_logical_forms;
	datalog_expression_root** test_logical_forms;
	if (!read_data(train_data, names, train_sentences, train_logical_forms, train_filepath)) {
		return false;
	} else if (extra_filepath != NULL
			&& !read_data(extra_data, names, extra_sentences, extra_logical_forms, extra_filepath)) {
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length); return false;
	} else if (kb_filepath != NULL
			&& !read_beliefs(kb_data, names, kb_logical_forms, kb_filepath)) {
		if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length); return false;
	} else if (!read_data(test_data, names, test_sentences, test_logical_forms, test_filepath)) {
		if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
		if (kb_filepath != NULL) cleanup(kb_data, NULL, kb_logical_forms, kb_data.length);
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length); return false;
	}

	syntax_node<datalog_expression_root>** syntax = (syntax_node<datalog_expression_root>**)
		calloc(train_data.length, sizeof(syntax_node<datalog_expression_root>*));
	if (syntax == NULL) {
		fprintf(stderr, "ERROR: Insufficient memory for syntax trees.\n");
		if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
		if (kb_filepath != NULL) cleanup(kb_data, NULL, kb_logical_forms, kb_data.length);
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length);
		cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
		return false;
	}

	FILE* in = fopen(input_filepath, "rb");
	hdp_grammar_type& G = *((hdp_grammar_type*) alloca(sizeof(hdp_grammar_type)));
	if (in == NULL) {
		fprintf(stderr, "ERROR: Unable to open '%s' for reading.\n", input_filepath);
		free(syntax); return false;
	} else {
		if (!read(G, syntax, train_data.length, in, names)) {
			fprintf(stderr, "ERROR: Unable to deserialize '%s'.\n", input_filepath);
			if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
			if (kb_filepath != NULL) cleanup(kb_data, NULL, kb_logical_forms, kb_data.length);
			cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
			cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
			return false;
		}
		fclose(in);
	}

	/* read the ontology */
	if ((ontology_filepath != NULL && !read_ontology(ontology, names, ontology_filepath))
	 || !ontology.initialize()) {
		if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
		if (kb_filepath != NULL) cleanup(kb_data, NULL, kb_logical_forms, kb_data.length);
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
		cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
		return false;
	}

	/* construct structures useful for printing derivation trees and logical forms */
	FILE* out = stdout;
	const string** name_ids = invert(names);
	const string** nonterminal_name_ids = invert(G.nonterminal_names);
	string_map_scribe terminal_printer = { name_ids, names.table.size + 1 };
	string_map_scribe nonterminal_printer = { nonterminal_name_ids, names.table.size + 1 };
debug_terminal_printer = &terminal_printer;
debug_nonterminal_printer = &nonterminal_printer;

	/* iterate over the train sentences and check that the logical forms can be parsed */
	for (unsigned int i = 0; test_parseability && i < train_data.length; i++) {
		datalog_expression_root logical_form_set;
		logical_form_set.index = NUMBER_ALL;
		logical_form_set.concord = NUMBER_NONE;
		logical_form_set.inf = INFLECTION_NONE;
debug_flag = (i == 467);
		if (!is_parseable(*syntax[i], *train_logical_forms[i], G,
				logical_form_set, nonterminal_printer, terminal_printer, name_ids))
		{
			printf("Sentence %u is not parseable:\n", i);
			print(*train_logical_forms[i], out, terminal_printer); print('\n', out);
			print(*syntax[i], out, nonterminal_printer, terminal_printer); print("\n\n", out);
		}
	}

	/* train the semantic prior */
	unsigned int old_train_count = train_data.length;
	train_logical_forms = (datalog_expression_root**) realloc(train_logical_forms,
			sizeof(datalog_expression_root*) * (train_data.length + extra_data.length));
	for (unsigned int i = 0; i < extra_data.length; i++) {
		train_logical_forms[train_data.length] = extra_logical_forms[i];
		train_data.add(extra_data[i]);
	}
	prior.train(train_logical_forms, train_data.length, kb_logical_forms, kb_data.length, 4, 10, 2);
	datalog_term_printer<string_map_scribe> prior_printer(terminal_printer);
	print_nonterminal_hdps(G, out, terminal_printer, nonterminal_printer);
	print(prior.edge_sampler, out, prior_printer, terminal_printer); print('\n', out);
	print(prior.edge_hdp.alpha, datalog_prior::EDGE_HDP_DEPTH + 1, out); print('\n', out);
	print(prior.constant_sampler, out, prior_printer, terminal_printer); print('\n', out);
	print(prior.constant_hdp.alpha, datalog_prior::CONSTANT_HDP_DEPTH + 1, out); print('\n', out);
	train_data.length = old_train_count;
	if (kb_filepath != NULL)
		cleanup(kb_data, NULL, kb_logical_forms, kb_data.length);

	/* type-check the logical forms */
	for (unsigned int i = 0; i < train_data.length; i++) {
		if (!type_check<true>(ontology, *train_logical_forms[i])) {
			printf("Logical form %u in the train set is not type-correct:\n", i);
			print(*train_logical_forms[i], out, terminal_printer); print('\n', out);
		}
	} for (unsigned int i = 0; i < test_data.length; i++) {
		if (!type_check<true>(ontology, *test_logical_forms[i])) {
			printf("Logical form %u in the test set is not type-correct:\n", i);
			print(*test_logical_forms[i], out, terminal_printer); print('\n', out);
		}
	}

	/* build a set of recognized tokens */
	hash_set<unsigned int> known_tokens = hash_set<unsigned int>(1024);
	for (const auto& N : G.nonterminals) {
		if (N.rule_distribution.type != PRETERMINAL) continue;
		for (const auto& entry : N.rule_distribution.h.pi.rules) {
			const rule<datalog_expression_root>& r = entry.key;
			for (unsigned int i = 0; i < r.length; i++) {
				if (!known_tokens.add(r.nonterminals[i])) {
					if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
					cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
					cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
					free(name_ids); free(nonterminal_name_ids); free(G); return false;
				}
			}
		}
		for (const auto& entry : N.rule_distribution.observations.counts) {
			const rule<datalog_expression_root>& r = entry.key;
			for (unsigned int i = 0; i < r.length; i++) {
				if (!known_tokens.add(r.nonterminals[i])) {
					if (extra_filepath != NULL) cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
					cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
					cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
					free(name_ids); free(nonterminal_name_ids); free(G); return false;
				}
			}
		}
	}

	/* create a thread pool and dispatch each thread to parse the test sentences */
	std::atomic_uint counter(0);
	std::atomic_uint incorrect(0);
	std::atomic_uint unanswered(0);
	std::thread* threads = new std::thread[thread_count];
	auto dispatch = [&]() {
		parse(G, test_sentences, test_data.length, test_logical_forms, known_tokens,
				terminal_printer, nonterminal_printer, out, unanswered, incorrect, counter, time_limit);
	};
	timer stopwatch;
	for (unsigned int i = 0; i < thread_count; i++)
		threads[i] = std::thread(dispatch);
	for (unsigned int i = 0; i < thread_count; i++)
		threads[i].join();
	unsigned int unanswered_count = unanswered;
	unsigned int incorrect_count = incorrect;
	fprintf(out, "Finished parsing. Time elapsed: %lfs\n", stopwatch.nanoseconds() * 1.0e-9);
	fprintf(out, "Number of unanswered sentences: %u\n", unanswered_count);
	fprintf(out, "Number of incorrect sentences: %u\n", incorrect_count);
	fprintf(out, "Test sentence count: %zu\n", test_data.length);
	delete[] threads;

	/* run a shell to parse user-input sentences */
	array<char> line = array<char>(256);
while (false) {
	//while (true) {
		printf("Enter sentence to parse:\n");
		int read = read_line(line, stdin);
		if (read == 0) {
			break;
		} else {
			for (char& c : line)
				c = tolower(c);
			array<unsigned int> tokens = array<unsigned int>(16);
			if (!tokenize(line.data, line.length, tokens, names)) {
				fprintf(stderr, "ERROR: Failed to tokenize sentence.\n");
				break;
			} else if (tokens.length == 0) {
				continue;
			} else if (tokens.last() != names.get("?")) {
				tokens.add(names.get("?"));
			}

			datalog_expression_root logical_form;
			syntax_node<datalog_expression_root>& parsed_syntax =
				*((syntax_node<datalog_expression_root>*) alloca(sizeof(syntax_node<datalog_expression_root>)));
			auto sentence = tokenized_sentence<datalog_expression_root>(sequence(tokens.data, tokens.length));
			if (parse<false>(parsed_syntax, logical_form, G, sentence, name_ids, time_limit)) {
				print(logical_form, out, terminal_printer); print('\n', out);
				print(parsed_syntax, out, nonterminal_printer, terminal_printer); print("\n", out);

				printf("Parse log probability: %lf (prior: %lf)\n",
						log_probability(G, parsed_syntax, logical_form),
						log_probability<MODE_PARSE, true>(logical_form));
				print('\n', out);
				free(logical_form); free(parsed_syntax);
			}
		}
		line.clear();
	}

	if (extra_filepath != NULL)
		cleanup(extra_data, extra_sentences, extra_logical_forms, extra_data.length);
	cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
	cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
	free(name_ids); free(nonterminal_name_ids); free(G);
	return true;
}

/* TODO: this is for testing; delete it */
bool get_predicates(const datalog_expression& exp, hash_set<unsigned int>& predicates) {
	switch (exp.type) {
	case DATALOG_PREDICATE:
		if (!predicates.add(exp.pred.function)) return false;
		for (unsigned int i = 0; i < array_length(exp.pred.args); i++) {
			if (exp.pred.args[i] == NULL) continue;
			if (!get_predicates(*exp.pred.args[i], predicates))
				return false;
		}
		return true;
	case DATALOG_FUNCTION:
		return get_predicates(*exp.func.arg, predicates);
	case DATALOG_TUPLE:
		for (unsigned int i = 0; i < exp.tuple.elements.length; i++)
			if (!get_predicates(*exp.tuple.elements[i], predicates)) return false;
		return true;
	case DATALOG_LIST:
		for (unsigned int i = 0; i < exp.list.elements.length; i++)
			if (!get_predicates(*exp.list.elements[i], predicates)) return false;
		return true;
	case DATALOG_VARIABLE:
	case DATALOG_CONSTANT:
	case DATALOG_INTEGER:
	case DATALOG_EMPTY:
	case DATALOG_ANY:
		return true;
	}
	fprintf(stderr, "get_predicates ERROR: Unrecognized datalog_expression type.\n");
	return false;
}

bool sample(hash_map<string, unsigned int>& names, const char* data_filepath,
		const char* lexicon_filepath, unsigned int iteration_count = 10,
		const char* output_filepath = NULL, const char* ontology_filepath = NULL,
		const char* grammar_filepath = "english.gram")
{
	disable_prior = true;
	array<datalog_expression_root*> data(1024), lexicon_data(1024);
	sequence* sentences; datalog_expression_root** logical_forms;
	sequence* lexicon_phrases; datalog_expression_root** lexicon_logical_forms = NULL;
	unsigned int* lexicon_nonterminals = NULL;
	if (!read_data(data, names, sentences, logical_forms, data_filepath)) {
		return false;
	} else if (lexicon_filepath != NULL
			&& !read_lexicon(lexicon_data, names, lexicon_phrases, lexicon_logical_forms, lexicon_filepath, lexicon_nonterminals)) {
		cleanup(data, sentences, logical_forms, data.length); return false;
	}

	hdp_grammar_type G;
	if (!read_grammar(G, names, grammar_filepath)) {
		cleanup(data, sentences, logical_forms, data.length);
		if (lexicon_filepath != NULL) cleanup(lexicon_data, lexicon_phrases, lexicon_logical_forms, lexicon_nonterminals, lexicon_data.length);
		return false;
	}

	/* read the ontology */
	if ((ontology_filepath != NULL && !read_ontology(ontology, names, ontology_filepath))
	 || !ontology.initialize()) {
		cleanup(data, sentences, logical_forms, data.length);
		if (lexicon_filepath != NULL) cleanup(lexicon_data, lexicon_phrases, lexicon_logical_forms, lexicon_nonterminals, lexicon_data.length);
	}

	/* construct structures useful for printing derivation trees and logical forms */
	FILE* out = stdout;
	const string** name_ids = invert(names);
	const string** nonterminal_name_ids = invert(G.nonterminal_names);
	string_map_scribe terminal_printer = { name_ids, names.table.size + 1 };
	string_map_scribe nonterminal_printer = { nonterminal_name_ids, names.table.size + 1 };
debug_terminal_printer = &terminal_printer;
debug_nonterminal_printer = &nonterminal_printer;

	/* add the lexicon to the HDPs */
	if (lexicon_filepath != NULL) {
		for (unsigned int i = 0; i < lexicon_data.length; i++) {
			syntax_node<datalog_expression_root> rule = syntax_node<datalog_expression_root>(lexicon_phrases[i]);

			bool contains;
			unsigned int nonterminal = G.nonterminal_names.get(*name_ids[lexicon_nonterminals[i]], contains);
			if (!contains || !add_tree(nonterminal, rule, *lexicon_logical_forms[i], G)) {
				fprintf(stderr, "ERROR: Unable to add lexicon item %u: ", i);
				print(*lexicon_logical_forms[i], stderr, terminal_printer); print(" with lexeme '", stderr);
				print(lexicon_phrases[i], stderr, terminal_printer); print("'.\n", stderr);
				cleanup(data, sentences, logical_forms, data.length);
				cleanup(lexicon_data, lexicon_phrases, lexicon_logical_forms, lexicon_nonterminals, lexicon_data.length);
				free(name_ids); free(nonterminal_name_ids); return false;
			}
		}
		cleanup(lexicon_data, lexicon_phrases, lexicon_logical_forms, lexicon_nonterminals, lexicon_data.length);
	}

	/* type-check the logical forms */
	for (unsigned int i = 0; i < data.length; i++) {
		if (!type_check<true>(ontology, *logical_forms[i])) {
			printf("Logical form %u is not type-correct:\n", i);
			print(*logical_forms[i], out, terminal_printer); print('\n', out);
		}
	}

	/* construct the initial derivation trees (running the parser with an empty grammar) */
	syntax_node<datalog_expression_root>** syntax = (syntax_node<datalog_expression_root>**)
			calloc(data.length, sizeof(syntax_node<datalog_expression_root>*));
	unsigned int* order = (unsigned int*) malloc(sizeof(unsigned int) * data.length);
	if (syntax == NULL || order == NULL) {
		fprintf(stderr, "ERROR: Insufficient memory for syntax trees.\n");
		if (syntax != NULL) free(syntax);
		cleanup(data, sentences, logical_forms, data.length);
		free(name_ids); free(nonterminal_name_ids); return false;
	}
	for (unsigned int i = 0; i < data.length; i++) order[i] = i;
	shuffle(order, (unsigned int) data.length);
	for (unsigned int i = 0; i < data.length; i++) {
		unsigned int id = order[i];
		auto sentence = tokenized_sentence<datalog_expression_root>(sentences[id]);
		syntax[id] = (syntax_node<datalog_expression_root>*) malloc(sizeof(syntax_node<datalog_expression_root>));
		if (syntax[id] == NULL
		 || !sample(syntax[id], G, *logical_forms[id], sentence, name_ids) || syntax[id] == NULL) /* sample can set syntax[id] to null */
		// || !parse<false>(*syntax[id], *logical_forms[id], G, sentence, name_ids) || syntax[id] == NULL) /* sample can set syntax[id] to null */
		{
			fprintf(stderr, "sample ERROR: Unable to sample derivation for sentence %u.\n", id);
			if (syntax[id] != NULL) { free(syntax[id]); syntax[id] = NULL; }
			cleanup(data, sentences, logical_forms, NULL, data.length, syntax);
			free(name_ids); free(nonterminal_name_ids); free(order); return false;
		}

		print(*logical_forms[id], out, terminal_printer); print('\n', out);
		print(*syntax[id], out, nonterminal_printer, terminal_printer); print("\n\n", out);

		if (!add_tree(1, *syntax[id], *logical_forms[id], G)) {
			cleanup(data, sentences, logical_forms, data.length, syntax);
			free(name_ids); free(nonterminal_name_ids); free(order); return false;
		}
	}
	fflush(out);

	/* perform inference */
	for (unsigned int t = 0; t < iteration_count; t++) {
		/* decrease the temperature by a bit */
		/*for (unsigned int i = 0; i < G.nonterminals.length; i++) {
			auto& rule_distribution = G.nonterminals[i].rule_distribution;
			for (unsigned int j = 0; j < G.nonterminals[i].feature_count + 1; j++)
				rule_distribution.a[j] = (rule_distribution.a[j] - 0.001) * 0.99 + 0.001;
		}*/

		shuffle(order, (unsigned int) data.length);
		for (unsigned int i = 0; i < data.length; i++) {
			//printf("[iteration %u] resampling sentence %u (ID: %u)\n", t, i, order[i]); fflush(stdout);
			auto sentence = tokenized_sentence<datalog_expression_root>(sentences[order[i]]);
			resample(syntax[order[i]], G, *logical_forms[order[i]], sentence, name_ids);
			//resample_locally(syntax[order[i]], G, logical_forms[order[i]], 2);
			//reparse<false>(syntax[order[i]], G, *logical_forms[order[i]], sentence, name_ids);
		}
		sample_grammar(G);
		fprintf(out, "Unnormalized log posterior probability: %lf\n",
				log_probability(G, syntax, logical_forms, data.length));

		if (t % 1 == 0) {
			fprintf(out, "[iteration %u]\n", t);
			print_nonterminal_hdps(G, out, terminal_printer, nonterminal_printer);
			printf("(seed = %u)\n", get_seed());
			fflush(out);
		}
	}

for (unsigned int i = 0; i < data.length; i++) {
print(*logical_forms[i], out, terminal_printer); print('\n', out);
print(*syntax[i], out, nonterminal_printer, terminal_printer); print("\n\n", out);
}

	if (output_filepath != NULL) {
		FILE* out = fopen(output_filepath, "wb");
		if (out == NULL) {
			printf("Unable to open state file for writing.\n");
		} else {
			printf("Saving state...");
			if (!write(G, syntax, data.length, out, name_ids)) {
				fprintf(stderr, "\nERROR: Unable to save state.\n");
			} else printf("done\n");
			fclose(out);
		}
		fflush(stdout);
	}

	cleanup(data, sentences, logical_forms, data.length, syntax);
	free(name_ids); free(nonterminal_name_ids); free(order);
	return true;
}

bool generate(
		hash_map<string, unsigned int>& names, const char* train_filepath,
		const char* test_filepath, const char* input_filepath,
		const char* ontology_filepath, unsigned int sentence_count = 20)
{
	array<datalog_expression_root*> train_data(1024), test_data(512);
	sequence* train_sentences;
	sequence* test_sentences;
	datalog_expression_root** train_logical_forms;
	datalog_expression_root** test_logical_forms;
	if (!read_data(train_data, names, train_sentences, train_logical_forms, train_filepath)) {
		return false;
	} else if (!read_data(test_data, names, test_sentences, test_logical_forms, test_filepath)) {
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length); return false;
	}

	syntax_node<datalog_expression_root>** syntax = (syntax_node<datalog_expression_root>**)
		calloc(train_data.length, sizeof(syntax_node<datalog_expression_root>*));
	if (syntax == NULL) {
		fprintf(stderr, "ERROR: Insufficient memory for syntax trees.\n");
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length);
		cleanup(test_data, test_sentences, test_logical_forms, test_data.length); return false;
	}

	FILE* in = fopen(input_filepath, "rb");
	hdp_grammar_type& G = *((hdp_grammar_type*) alloca(sizeof(hdp_grammar_type)));
	if (in == NULL) {
		fprintf(stderr, "ERROR: Unable to open '%s' for reading.\n", input_filepath);
		free(syntax); return false;
	} else {
		if (!read(G, syntax, train_data.length, in, names)) {
			fprintf(stderr, "ERROR: Unable to deserialize '%s'.\n", input_filepath);
			cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
			cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
			return false;
		}
		fclose(in);
	}

	/* read the ontology */
	if ((ontology_filepath != NULL && !read_ontology(ontology, names, ontology_filepath))
	 || !ontology.initialize()) {
		cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
		cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
		return false;
	}

	/* construct structures useful for printing derivation trees and logical forms */
	FILE* out = stdout;
	const string** name_ids = invert(names);
	const string** nonterminal_name_ids = invert(G.nonterminal_names);
	string_map_scribe terminal_printer = { name_ids, names.table.size + 1 };
	string_map_scribe nonterminal_printer = { nonterminal_name_ids, names.table.size + 1 };

	/* type-check the logical forms */
	for (unsigned int i = 0; i < train_data.length; i++) {
		if (!type_check<true>(ontology, *train_logical_forms[i])) {
			printf("Logical form %u in the train set is not type-correct:\n", i);
			print(*train_logical_forms[i], out, terminal_printer); print('\n', out);
		}
	} for (unsigned int i = 0; i < test_data.length; i++) {
		if (!type_check<true>(ontology, *test_logical_forms[i])) {
			printf("Logical form %u in the test set is not type-correct:\n", i);
			print(*test_logical_forms[i], out, terminal_printer); print('\n', out);
		}
	}

	/* generate the sentences */
	double total_score = 0.0;
	for (unsigned int i = 0; i < test_data.length; i++) {
		sequence sampled_sentence = sequence(NULL, 0);
		hash_map<sequence, unsigned int> counts = hash_map<sequence, unsigned int>(64);
		//for (unsigned int j = 0; j < sentence_count; j++) {
		while (true) {
			syntax_node<datalog_expression_root>* sampled_tree;
			//fprintf(out, "(%u) Sampled derivation tree %u:\n", i, j);

			int success;
			do {
				success = sample(G, sampled_tree, *test_logical_forms[i]);
				if (success == -1) {
					fprintf(stderr, "ERROR: Failed to sample sentence.\n");
					break;
				}
			} while (success != 0);
			if (success == -1) break;

			sequence sentence = sequence(NULL, 0);
			//print(*sampled_tree, out, nonterminal_printer, terminal_printer); print('\n', out);
			if (!yield(G, *sampled_tree, *test_logical_forms[i], sentence) || !counts.check_size()) {
				cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
				cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
				for (auto entry : counts) { free(entry.key); } free(*sampled_tree); free(sampled_tree);
				return false;
			}
			free(*sampled_tree); free(sampled_tree);

			bool contains; unsigned int bucket;
			unsigned int& count = counts.get(sentence, contains, bucket);
			if (!contains) {
				counts.table.keys[bucket] = sentence;
				counts.table.size++;
				count = 1;
			} else {
				free(sentence);
				count++;
			}

			if (count == sentence_count) {
				sampled_sentence = counts.table.keys[bucket];
				break;
			}
		}

		double score = bleu(test_sentences[i], sampled_sentence);
		fprintf(out, "(%u) Sampled sentence: ", i);
		print(sampled_sentence, out, terminal_printer); print('\n', out);
		fprintf(out, "  BLEU: %lf\n", score);
		for (auto entry : counts) free(entry.key);
		free(sampled_sentence);

		total_score += score;
	}
	free(name_ids); free(nonterminal_name_ids);

	fprintf(out, "Average BLEU: %lf\n", total_score / test_data.length);
	cleanup(train_data, train_sentences, train_logical_forms, train_data.length, syntax);
	cleanup(test_data, test_sentences, test_logical_forms, test_data.length);
	return true;
}

enum command {
	COMMAND_SAMPLE,
	COMMAND_PARSE,
	COMMAND_GENERATE
};

inline bool parse_option(const char* arg, const char* to_match) {
	return (strcmp(arg, to_match) == 0);
}

inline bool parse_option(const char* arg,
		const char* to_match, const char*& dst)
{
	unsigned int length = strlen(to_match);
	if (strncmp(arg, to_match, length) != 0
	 || arg[length] != '=')
		return false;
	dst = arg + length + 1;
	return true;
}

int main(int argc, const char** argv)
{
set_seed(81793920);
	command cmd;
	if (argc < 2) {
		fprintf(stderr, "Not enough arguments.\n");
		return EXIT_FAILURE;
	} else if (strcmp(argv[1], "sample") == 0) {
		cmd = COMMAND_SAMPLE;
	} else if (strcmp(argv[1], "parse") == 0) {
		cmd = COMMAND_PARSE;
	} else if (strcmp(argv[1], "generate") == 0) {
		cmd = COMMAND_GENERATE;
	} else {
		fprintf(stderr, "Unrecognized command '%s'.\n", argv[1]);
		return EXIT_FAILURE;
	}

	/* parse the command-line arguments */
	const char* train_filepath = NULL;
	const char* extra_filepath = NULL;
	const char* kb_filepath = NULL;
	const char* test_filepath = NULL;
	const char* model_filepath = NULL;
	const char* ontology_filepath = NULL;
	const char* grammar_filepath = "english.gram";
	const char* iterations_arg = "10";
	const char* thread_count_arg = "1";
	const char* time_limit_arg = "-1";
	const char* agid_filepath = "infl.txt";
	const char* uncountable_filepath = "uncountable.txt";
	bool test_parseability = false;
	for (int i = 2; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] != '-') {
			fprintf(stderr, "Invalid command-line option: '%s'.\n", argv[i]);
			return EXIT_FAILURE;
		}

		if (parse_option(argv[i] + 2, "train", train_filepath)) continue;
		if (parse_option(argv[i] + 2, "extra", extra_filepath)) continue;
		if (parse_option(argv[i] + 2, "kb", kb_filepath)) continue;
		if (parse_option(argv[i] + 2, "test", test_filepath)) continue;
		if (parse_option(argv[i] + 2, "model", model_filepath)) continue;
		if (parse_option(argv[i] + 2, "ontology", ontology_filepath)) continue;
		if (parse_option(argv[i] + 2, "grammar", grammar_filepath)) continue;
		if (parse_option(argv[i] + 2, "iterations", iterations_arg)) continue;
		if (parse_option(argv[i] + 2, "time-limit", time_limit_arg)) continue;
		if (parse_option(argv[i] + 2, "threads", thread_count_arg)) continue;
		if (parse_option(argv[i] + 2, "no-morphology")) { enable_morphology = false; continue; }
		if (parse_option(argv[i] + 2, "test-parseable")) { test_parseability = true; continue; }
		if (parse_option(argv[i] + 2, "agid", agid_filepath)) continue;
		if (parse_option(argv[i] + 2, "uncountable", uncountable_filepath)) continue;
		else {
			fprintf(stderr, "Unrecognized command-line option: '%s'.\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	char* endptr;
	unsigned int iteration_count = strtol(iterations_arg, &endptr, 0);
	if (*endptr != '\0') {
		fprintf(stderr, "Unable to interpret iteration count argument.\n"); return EXIT_FAILURE;
	}
	unsigned int time_limit = UINT_MAX;
	if (time_limit_arg != NULL) {
		if (strcmp(time_limit_arg, "-1") == 0) {
			time_limit = UINT_MAX;
		} else {
			time_limit = strtol(time_limit_arg, &endptr, 0);
			if (*endptr != '\0') {
				fprintf(stderr, "Unable to interpret time limit argument.\n"); return EXIT_FAILURE;
			}
		}
	}
	unsigned int thread_count = strtol(thread_count_arg, &endptr, 0);
	if (*endptr != '\0') {
		fprintf(stderr, "Unable to interpret thread count argument.\n"); return EXIT_FAILURE;
	}
	if (train_filepath == NULL) {
		fprintf(stderr, "Train filepath unspecified.\n"); return EXIT_FAILURE;
	} if (test_filepath == NULL && cmd == COMMAND_PARSE) {
		fprintf(stderr, "Test filepath unspecified.\n"); return EXIT_FAILURE;
	} if (model_filepath == NULL) {
		fprintf(stderr, "Trained model filepath unspecified.\n"); return EXIT_FAILURE;
	}

	/* initialize the token map and morphology model */
	hash_map<string, unsigned int> names(1024);
	if (!populate_name_map(names) || !morph.initialize(names)) {
		for (auto entry : names) { free(entry.key); }
		return EXIT_FAILURE;
	}

	/* read the word inflection database */
	if (enable_morphology && !morphology_read(morph, names, agid_filepath, uncountable_filepath)) {
		for (auto entry : names) { free(entry.key); }
		return EXIT_FAILURE;
	}

	/* run the commands */
	switch (cmd) {
	case COMMAND_PARSE:
		parse(names, train_filepath, extra_filepath, kb_filepath, test_filepath, model_filepath, ontology_filepath, time_limit, thread_count, test_parseability); break;
	case COMMAND_SAMPLE:
		sample(names, train_filepath, extra_filepath, iteration_count, model_filepath, ontology_filepath, grammar_filepath); break;
	case COMMAND_GENERATE:
		generate(names, train_filepath, test_filepath, model_filepath, ontology_filepath); break;
	default:
		fprintf(stderr, "main ERROR: Unrecognized command.\n"); break;
	}

	for (auto entry : names) { free(entry.key); }
	return EXIT_SUCCESS;
}

import bisect
import hashlib
import json
import logging
import os
import shutil
import time

from policy import PropertyArray, PropertyMultiArray, dict_to_properties, ImmutablePropertyError

logging.basicConfig(format='[%(levelname)s]: %(message)s', level=logging.DEBUG)

POLICY_DIR = "pib/examples/"
PIB_EXTENSIONS = ('.policy', '.profile', '.pib')


def load_policy_json(filename):
    """Read and decode a .policy JSON file and return a NEATPolicy object."""
    try:
        policy_file = open(filename, 'r')
        policy_dict = json.load(policy_file)
    except OSError as e:
        logging.error('Policy ' + filename + ' not found.')
        return
    except json.decoder.JSONDecodeError as e:
        logging.error('Error parsing policy file ' + filename)
        print(e)
        return

    p = NEATPolicy(policy_dict)
    return p


class NEATPolicy(object):
    """NEAT policy representation"""

    def __init__(self, policy_dict, policy_file=None):
        # set default values

        # TODO do we need to handle unknown attributes?
        for k, v in policy_dict.items():
            if isinstance(v, str):
                setattr(self, k, v)

        self.priority = int(policy_dict.get('priority', 0))
        self.replace_matched = policy_dict.get('replace_matched', False)

        self.filename = None
        self.time = time.time()

        # parse match fields
        match = policy_dict.get('match', {})
        self.match = PropertyArray()
        self.match.add(*dict_to_properties(match))

        # parse augment properties
        properties = policy_dict.get('properties', {})
        self.properties = PropertyMultiArray()
        self.properties.add(*dict_to_properties(properties))

        # set UID
        self.uid = policy_dict.get('uid')
        if self.uid is None:
            self.uid = self.__gen_uid()
        else:
            self.uid = str(self.uid).lower()

        # deprecated
        self.name = policy_dict.get('name', self.uid)

    def __gen_uid(self):
        # TODO make UID immutable?
        s = str(id(self))
        return hashlib.md5(s.encode('utf-8')).hexdigest()

    def dict(self):
        d = {}
        for attr in ['uid', 'priority', 'replace_matched', 'filename', 'time']:
            try:
                d[attr] = getattr(self, attr)
            except AttributeError:
                logging.warning("Policy doesn't contain attribute %s" % attr)

        d['match'] = self.match.dict()
        d['properties'] = self.properties.dict()

        return d

    def json(self):
        return json.dumps(self.dict(), indent=4, sort_keys=True)

    def match_len(self):
        """Use the number of match elements to sort the entries in the PIB.
        Entries with the smallest number of elements are matched first."""
        return len(self.match)

    def match_query(self, input_properties, strict=True):
        """Check if the match properties are completely covered by the properties of a query.

        If strict flag is set match only properties with precedences that are higher or equal to the precedence
        of the corresponding match property.
        """

        # always return True if the match field is empty (wildcard)
        if not self.match:
            return True

        # TODO check
        # find full overlap?
        if not self.match.items() <= input_properties.items():
            return

        # find intersection
        matching_props = self.match.items() & input_properties.items()

        if strict:
            # ignore properties with a lower precedence than the associated match property
            return bool({k for k, v in matching_props if input_properties[k].precedence >= self.match[k].precedence})
        else:
            return bool(matching_props)

    def apply(self, properties: PropertyArray):
        """Apply policy properties to a set of candidate properties."""
        for p in self.properties.values():
            logging.info("applying property %s" % p)
            properties.add(*p)

    def __str__(self):
        return "%d POLICY %s: %s   ==>   %s" % (self.priority, self.uid, self.match, self.properties)

    def __repr__(self):
        return repr({a: getattr(self, a) for a in ['uid', 'match', 'properties', 'priority']})


class PIB(list):
    def __init__(self, policy_dir, file_extension=('.policy', '.profile'), policy_type='policy'):
        super().__init__()
        self.policies = self
        self.index = {}

        self.file_extension = file_extension
        # track PIB files

        self.policy_type = policy_type
        self.policy_dir = policy_dir
        self.load_policies(self.policy_dir)

    @property
    def files(self):
        return {v.filename: v for uid, v in self.index.items()}

    def load_policies(self, policy_dir=None):
        """Load all policies in policy directory."""
        if not policy_dir:
            policy_dir = self.policy_dir;

        for filename in os.listdir(policy_dir):
            if filename.endswith(self.file_extension) and not filename.startswith(('.', '#')):
                self.load_policy(os.path.join(policy_dir, filename))

    def load_policy(self, filename):
        """Load policy.
        """
        if not filename.endswith(self.file_extension) and filename.startswith(('.', '#')):
            return
        stat = os.stat(filename)

        t = stat.st_mtime_ns
        if filename not in self.files or self.files[filename].timestamp != t:
            logging.info("Loading policy %s...", filename)
            p = load_policy_json(filename)
            # update filename and timestamp
            p.filename = filename
            p.timestamp = t
            if p:
                self.register(p)
        else:
            pass
            # logging.debug("Policy %s is up-to-date", filename)

    def reload(self):
        """
        Reload PIB files
        """
        current_files = set()

        for dir_path, dir_names, filenames in os.walk(self.policy_dir):
            for f in filenames:
                full_name = os.path.join(dir_path, f)
                current_files.add(full_name)
                self.load_policy(full_name)

        # check if any files were deleted
        deleted_files = self.files.keys() - current_files

        for f in deleted_files:
            logging.info("Policy file %s has been deleted", f)
            # unregister policy
            self.unregister(self.files[f].uid)

    def register(self, policy):
        """Register new policy

        Policies are ordered
        """
        # check for existing policies with identical match properties
        if policy.match in [p.match for p in self.policies]:
            logging.debug("Policy match fields for policy %s already registered. " % (policy.uid))
            # return

        # TODO tie breaker using match_len?
        uid = bisect.bisect([p.priority for p in self.policies], policy.priority)
        self.policies.insert(uid, policy)

        # self.policies.sort(key=operator.methodcaller('match_len'))
        self.index[policy.uid] = policy

    def unregister(self, policy_uid):
        del self.index[policy_uid]

    def lookup(self, input_properties, apply=True, cand_id=None):
        """
        Look through all installed policies to find the ones which match the properties of the given candidate.
        If apply is True, append the matched policy properties.

        Returns all matched policies.
        """

        assert isinstance(input_properties, PropertyArray)
        if cand_id is None:
            cand_id = ""

        logging.info("matching policies for candidate %s" % cand_id)

        candidates = [input_properties]

        for p in self.policies:
            if p.match_query(input_properties):
                tmp_candidates = []
                policy_info = str(p.uid)
                if hasattr(p, "description"):
                    policy_info += ': %s' % p.description
                logging.info("    " + policy_info)
                if apply:
                    while candidates:
                        candidate = candidates.pop()
                        # if replace_matched was set, remove any occurrence of match properties from the candidate
                        if p.replace_matched:
                            for key in p.match:
                                del candidate[key]
                                logging.debug('    removing property:' + key)

                        for policy_properties in p.properties.expand():
                            try:
                                new_candidate = candidate + policy_properties
                            except ImmutablePropertyError:
                                continue
                            # TODO copy policies from candidate and policy_properties for debugging
                            #  if hasattr(new_candidate, 'policies'):
                            #      new_candidate.policies.append(p.uid)
                            #  else:
                            #      new_candidate.policies = [p.uid]
                            tmp_candidates.append(new_candidate)
                candidates.extend(tmp_candidates)
        return candidates

    def dump(self):
        ts = shutil.get_terminal_size()
        tcol = ts.columns
        s = "=" * int((tcol - 11) / 2) + " PIB START " + "=" * int((tcol - 11) / 2) + "\n"
        for p in self.policies:
            s += str(p) + '\n'
        s += "=" * int((tcol - 9) / 2) + " PIB END " + "=" * int((tcol - 9) / 2) + "\n"
        print(s)


if __name__ == "__main__":
    pib = PIB('pib/examples/')
    pib.dump()

    import code

    code.interact(local=locals(), banner='PIB loaded:')
